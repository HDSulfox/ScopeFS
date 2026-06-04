$ErrorActionPreference = "Stop"

$exe = Join-Path $PSScriptRoot "..\build\scopefs.exe"
if (!(Test-Path $exe)) {
  throw "scopefs.exe not found. Build first with: cmake --build build -j 4"
}

function Run-ScopeText([string]$Text) {
  $psi = [System.Diagnostics.ProcessStartInfo]::new()
  $psi.FileName = (Resolve-Path $exe).Path
  $psi.Arguments = "--script"
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $proc = [System.Diagnostics.Process]::Start($psi)
  $proc.StandardInput.Write($Text)
  $proc.StandardInput.Close()
  $stdout = $proc.StandardOutput.ReadToEnd()
  $stderr = $proc.StandardError.ReadToEnd()
  $proc.WaitForExit()
  $output = $stdout + $stderr
  $output | Write-Host
  if ($proc.ExitCode -ne 0) {
    throw "scopefs exited with $($proc.ExitCode)"
  }
  return $output
}

function Parse-DirRows([string]$Output, [string]$Name) {
  $prefix = "$Name`t"
  $rows = @()
  foreach ($line in ($Output -split "\r?\n")) {
    if (!$line.StartsWith($prefix)) { continue }
    $parts = $line -split "`t"
    if ($parts.Length -lt 11) {
      throw "dir row for $Name had $($parts.Length) columns: $line"
    }
    $rows += [pscustomobject]@{
      Name = $parts[0]
      Type = $parts[1]
      Size = $parts[2]
      Owner = $parts[3]
      Group = $parts[4]
      Mode = $parts[5]
      Inode = ($parts[6] -replace "^index node ", "")
      Version = ($parts[7] -replace "^version ", "")
      ReferenceCount = ($parts[8] -replace "^reference count ", "")
      Created = ($parts[9] -replace "^created ", "")
      Modified = ($parts[10] -replace "^modified ", "")
    }
  }
  return @($rows)
}

function Assert-True([bool]$Condition, [string]$Message) {
  if (!$Condition) { throw $Message }
}

function Assert-Time([string]$Value, [string]$Label) {
  Assert-True ($Value -match "^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$") "$Label was not a display timestamp: $Value"
}

function Read-Time([string]$Value) {
  $culture = [System.Globalization.CultureInfo]::InvariantCulture
  return [datetime]::ParseExact($Value, "yyyy-MM-dd HH:mm", $culture)
}

Write-Host "== old inode time field compatibility =="
$legacySetup = @"
lang en
format
login root root
create /legacy 0644
exit
"@
Run-ScopeText $legacySetup | Out-Null

$volume = Join-Path $PSScriptRoot "..\.scopefs\scopefs.volume"
$oldLines = @()
foreach ($line in [System.IO.File]::ReadAllLines((Resolve-Path $volume).Path)) {
  if ($line.StartsWith("I|")) {
    $fields = $line -split "\|"
    if ($fields.Length -ge 15) {
      $oldLines += (($fields[0..12]) -join "|")
      continue
    }
  }
  $oldLines += $line
}
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllLines((Resolve-Path $volume).Path, $oldLines, $utf8NoBom)

$legacyOutput = Run-ScopeText @"
lang en
login root root
dir /
exit
"@
$legacyRows = @(Parse-DirRows $legacyOutput "legacy")
Assert-True ($legacyRows.Count -eq 1) "legacy row missing after old-format volume load"
Assert-True ($legacyRows[0].Created -eq "-") "legacy created time should display '-'"
Assert-True ($legacyRows[0].Modified -eq "-") "legacy modified time should display '-'"

Write-Host "== dir time and metadata semantics =="
$mainOutput = Run-ScopeText @"
lang en
format
login root root
create /file 0644
dir /
open /file rw
write 3 abc
close 3
dir /
chmod /file 0600
chclass /file system
acl grant /file usr1 r path_prefix=/
dir /
truncate /file 1
dir /
mkdir /dir
dir /
rmdir /dir
dir /
cp /file /copy
dir /
clone /file /legacy-copy
exit
"@

$expectedHeader = "name`ttype`tsize`towner`tgroup`tmode`tindex node`tversion`treference count`tcreated`tmodified"
Assert-True ($mainOutput.Contains($expectedHeader)) "dir header did not include expanded time and metadata columns"
Assert-True ($mainOutput -match "size 3B") "dir output did not include B-sized file data"
Assert-True ($mainOutput -match "index node \d+") "dir output missing index node label"
Assert-True ($mainOutput -match "version \d+") "dir output missing version label"
Assert-True ($mainOutput -match "reference count \d+") "dir output missing reference count label"
Assert-True ($mainOutput -match "copied /file -> /copy") "cp command did not report copy result"
Assert-True ($mainOutput -match "unknown command: clone") "clone should no longer be accepted as a user command"

$fileRows = @(Parse-DirRows $mainOutput "file")
Assert-True ($fileRows.Count -ge 7) "expected repeated file rows in dir output"
foreach ($row in $fileRows) {
  Assert-Time $row.Created "file created"
  Assert-Time $row.Modified "file modified"
}

$created0 = $fileRows[0].Created
$modified0 = Read-Time $fileRows[0].Modified
$writeModified = Read-Time $fileRows[1].Modified
$metadataModified = Read-Time $fileRows[2].Modified
$truncateModified = Read-Time $fileRows[3].Modified

Assert-True ($fileRows[1].Created -eq $created0) "write changed file creation time"
Assert-True ($writeModified -gt $modified0) "write did not advance modified time"
Assert-True ($metadataModified -eq $writeModified) "chmod/chclass/acl changed modified time"
Assert-True ($truncateModified -gt $metadataModified) "truncate did not advance modified time"
Assert-True ($fileRows[3].Size -eq "size 1B") "truncate did not update B-sized dir output"

$rootRows = @(Parse-DirRows $mainOutput ".")
Assert-True ($rootRows.Count -ge 6) "expected repeated root rows in dir output"
$beforeMkdir = Read-Time $rootRows[3].Modified
$afterMkdir = Read-Time $rootRows[4].Modified
$afterRmdir = Read-Time $rootRows[5].Modified
Assert-True ($afterMkdir -gt $beforeMkdir) "mkdir did not advance parent directory modified time"
Assert-True ($afterRmdir -gt $afterMkdir) "rmdir did not advance parent directory modified time"

$copyRows = @(Parse-DirRows $mainOutput "copy")
Assert-True ($copyRows.Count -eq 1) "cp target row missing"
Assert-Time $copyRows[0].Created "copy created"
Assert-Time $copyRows[0].Modified "copy modified"
Assert-True ($copyRows[0].Created -eq $copyRows[0].Modified) "new cp inode should start with matching created/modified times"

Write-Host "Time attribute checks passed."
