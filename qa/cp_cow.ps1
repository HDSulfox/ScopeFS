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
  if ($output -match "(?m)^E_") {
    throw "scopefs emitted an unexpected error"
  }
  return $output
}

function Assert-True([bool]$Condition, [string]$Message) {
  if (!$Condition) { throw $Message }
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
      ReferenceCount = ($parts[8] -replace "^reference count ", "")
    }
  }
  return @($rows)
}

function Decode-Hex([string]$Hex) {
  if ([string]::IsNullOrEmpty($Hex)) { return "" }
  $bytes = New-Object byte[] ($Hex.Length / 2)
  for ($i = 0; $i -lt $bytes.Length; ++$i) {
    $bytes[$i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16)
  }
  return [System.Text.Encoding]::UTF8.GetString($bytes)
}

function Parse-BlockList([string]$Csv) {
  if ([string]::IsNullOrWhiteSpace($Csv)) { return @() }
  return @($Csv -split "," | Where-Object { $_ -ne "" } | ForEach-Object { [uint32]$_ })
}

function Read-VolumeState() {
  $volume = Join-Path $PSScriptRoot "..\.scopefs\scopefs.volume"
  if (!(Test-Path $volume)) {
    throw "scopefs volume not found: $volume"
  }

  $state = [pscustomobject]@{
    ActiveRoot = ""
    Inodes = @{}
    Blocks = @{}
    Entries = @{}
  }

  foreach ($line in (Get-Content $volume)) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $parts = $line -split "\|", -1
    if ($parts[0] -eq "S" -and $parts.Length -ge 13) {
      $state.ActiveRoot = $parts[7]
    } elseif ($parts[0] -eq "I" -and $parts.Length -ge 15) {
      $state.Inodes[$parts[1]] = [pscustomobject]@{
        Id = $parts[1]
        Type = $parts[3]
        Blocks = @(Parse-BlockList $parts[12])
      }
    } elseif ($parts[0] -eq "D" -and $parts.Length -ge 6) {
      $parent = $parts[1]
      if (!$state.Entries.ContainsKey($parent)) {
        $state.Entries[$parent] = @{}
      }
      $name = Decode-Hex $parts[2]
      $state.Entries[$parent][$name] = [pscustomobject]@{
        Name = $name
        Type = $parts[3]
        Inode = $parts[4]
      }
    } elseif ($parts[0] -eq "B" -and $parts.Length -ge 8) {
      $state.Blocks[[uint32]$parts[1]] = [pscustomobject]@{
        Id = [uint32]$parts[1]
        Refcount = [uint32]$parts[2]
        OwnerInode = [uint32]$parts[6]
      }
    }
  }

  return $state
}

function Get-RootEntry($State, [string]$Name) {
  $entries = $State.Entries[$State.ActiveRoot]
  if ($null -eq $entries -or !$entries.ContainsKey($Name)) {
    return $null
  }
  return $entries[$Name]
}

Write-Host "== cp COW block sharing =="
$data = "abcdefghijklmnopqrstuvwxyz0123456789"
$setup = Run-ScopeText @"
format
lang en
login root root
mkdir /parent
mkdir /parent/child
create /a 0644
open /a rw
write 3 $data
close 3
cp /a /b
dir /
dir /parent/child
trace 40
fsck
exit
"@

Assert-True ($setup -match "block\.retain") "cp did not retain any existing data block"
Assert-True ($setup -match "inode\.clone") "cp did not clone the source inode"
Assert-True ($setup -match "fsck: clean") "fsck was not clean after cp"

$sourceRows = @(Parse-DirRows $setup "a")
$copyRows = @(Parse-DirRows $setup "b")
$parentRows = @(Parse-DirRows $setup "..")
Assert-True ($sourceRows.Count -eq 1) "dir output did not include source row"
Assert-True ($copyRows.Count -eq 1) "dir output did not include copy row"
Assert-True ($sourceRows[0].ReferenceCount -eq "2") "cp source dir reference count expected 2 got $($sourceRows[0].ReferenceCount)"
Assert-True ($copyRows[0].ReferenceCount -eq "2") "cp copy dir reference count expected 2 got $($copyRows[0].ReferenceCount)"
Assert-True (@($parentRows | Where-Object { $_.ReferenceCount -eq "3" }).Count -ge 1) "dir .. reference count should show parent directory link count 3"

$state = Read-VolumeState
$source = Get-RootEntry $state "a"
$copy = Get-RootEntry $state "b"
Assert-True ($null -ne $source) "source file /a missing after cp setup"
Assert-True ($null -ne $copy) "copy file /b missing after cp"

$sourceInode = $state.Inodes[$source.Inode]
$copyInode = $state.Inodes[$copy.Inode]
Assert-True ($null -ne $sourceInode) "source inode missing"
Assert-True ($null -ne $copyInode) "copy inode missing"
Assert-True ($sourceInode.Type -eq "regular") "source inode is not regular"
Assert-True ($copyInode.Type -eq "regular") "copy inode is not regular"
Assert-True ($sourceInode.Blocks.Count -gt 0) "source file has no data blocks"
Assert-True (($sourceInode.Blocks -join ",") -eq ($copyInode.Blocks -join ",")) "cp used different data blocks instead of shared COW blocks"

foreach ($block in $sourceInode.Blocks) {
  Assert-True ($state.Blocks.ContainsKey($block)) "shared block $block is missing"
  Assert-True ($state.Blocks[$block].Refcount -eq 2) "shared block $block refcount expected 2 got $($state.Blocks[$block].Refcount)"
}

Write-Host "== delete source keeps copied data =="
$afterDelete = Run-ScopeText @"
lang en
login root root
delete /a
open /b r
read 3 128
close 3
dir /
fsck
exit
"@

Assert-True ($afterDelete -match [regex]::Escape($data)) "copy data was not readable after deleting source"
Assert-True ($afterDelete -match "fsck: clean") "fsck was not clean after deleting source"
$copyRowsAfterDelete = @(Parse-DirRows $afterDelete "b")
Assert-True ($copyRowsAfterDelete.Count -eq 1) "dir output did not include copy row after deleting source"
Assert-True ($copyRowsAfterDelete[0].ReferenceCount -eq "1") "copy dir reference count expected 1 after deleting source got $($copyRowsAfterDelete[0].ReferenceCount)"

$state = Read-VolumeState
$source = Get-RootEntry $state "a"
$copy = Get-RootEntry $state "b"
Assert-True ($null -eq $source) "source file /a still exists after delete"
Assert-True ($null -ne $copy) "copy file /b missing after deleting source"

$copyInode = $state.Inodes[$copy.Inode]
Assert-True (($copyInode.Blocks -join ",") -eq ($sourceInode.Blocks -join ",")) "copy changed block map after deleting source"
foreach ($block in $copyInode.Blocks) {
  Assert-True ($state.Blocks.ContainsKey($block)) "copy block $block was reclaimed while still referenced"
  Assert-True ($state.Blocks[$block].Refcount -eq 1) "copy block $block refcount expected 1 after deleting source got $($state.Blocks[$block].Refcount)"
}

Write-Host "cp COW checks passed."
