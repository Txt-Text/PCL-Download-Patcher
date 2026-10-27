# 本脚本只生成和修改新输出目录中的合成样本，不修改 PCL 样本，也不运行 Git。
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('output/behavior-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$csc = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$ilasm = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/ilasm.exe'
$ildasm = Join-Path ${env:ProgramFiles(x86)} 'Microsoft SDKs/Windows/v10.0A/bin/NETFX 4.8 Tools/ildasm.exe'
$verify = Join-Path (Split-Path $ildasm) 'PEVerify.exe'
$regression = Join-Path $PSScriptRoot 'output/Regression.exe'
if (Test-Path -LiteralPath $OutputDirectory) { throw '输出目录已经存在；请使用一个新的目录。' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null

function Invoke-Checked([string]$Tool, [string[]]$Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed: $LASTEXITCODE" }
}

function Replace-Method([string]$Text, [string]$MethodMarker, [string]$Replacement) {
    $endMarker = '} // end of method ModModpack::ModpackInstall'
    $position = 0
    while (($position = $Text.IndexOf($endMarker, $position)) -ge 0) {
        $start = $Text.LastIndexOf('  .method ', $position)
        if ($start -lt 0) { throw '无法定位合成方法的开头。' }
        $length = $position + $endMarker.Length - $start
        $method = $Text.Substring($start, $length)
        if ($method.Contains($MethodMarker)) { return $Text.Remove($start, $length).Insert($start, $Replacement) }
        $position += $endMarker.Length
    }
    throw "未找到方法标记：$MethodMarker"
}

$entry = @'
  .method public hidebysig static void ModpackInstall() cil managed {
    .maxstack 3
    .locals init (class PCL.ModModpack/EntryClosure closure)
    newobj instance void PCL.ModModpack/EntryClosure::.ctor()
    stloc.0
    ldstr "为遵守相关法律法规，PCL 暂时不再提供 MC 下载功能，非常抱歉 qwq……"
    ldc.i4.2
    ldc.i4.1
    call void PCL.ModMain::Hint(string, valuetype PCL.ModMain/HintType, bool)
    ret
  } // end of method ModModpack::ModpackInstall
'@

foreach ($variant in @('ordinary', 'reordered')) {
    $directory = Join-Path $OutputDirectory $variant
    New-Item -ItemType Directory -Path $directory | Out-Null
    $seed = Join-Path $directory 'seed.exe'
    $compileArguments = @('/nologo', '/optimize+', '/target:exe', '/main:PCL.RecoveryProgram', "/out:$seed",
        '/r:System.IO.Compression.dll', '/r:System.IO.Compression.FileSystem.dll', (Join-Path $PSScriptRoot 'RecoveryFixture.cs'))
    if ($variant -eq 'reordered') { $compileArguments += '/define:REORDER' }
    Invoke-Checked $csc $compileArguments
    $ilFile = Join-Path $directory 'fixture.il'
    Invoke-Checked $ildasm @($seed, '/text', '/nobar', '/utf8', "/out:$ilFile")
    $text = [IO.File]::ReadAllText($ilFile)
    $text = Replace-Method $text 'ModpackInstall()' $entry
    # 语义相同但格式编号不同，验证分发编号来自目标的 GetEntry 分支。
    $values = if ($variant -eq 'ordinary') { @(0, 1, 2, 3, 4, 9) } else { @(107, 213, 111, 55, 32, 79) }
    $markers = @('manifest.json', 'modpack.json', 'mmc-pack.json', 'mcbbs.packmeta', 'modrinth.index.json', 'modpack.zip')
    $body = @'
  .method public hidebysig static class PCL.ModLoader/LoaderCombo`1<string>
      ModpackInstall(string file, string instance_name, string logo) cil managed {
    .maxstack 3
    .locals init (class PCL.ModLoader/LoaderCombo`1<string> result,
      class [System.IO.Compression]System.IO.Compression.ZipArchive zip,
      string prefix, int32 pack,
      valuetype [mscorlib]System.Threading.CancellationToken cancellation)
    ldnull
    stloc.1
    ldstr ""
    stloc.2
    ldc.i4.m1
    stloc.3
    .try {
      ldarg.0
      ldloca.s cancellation
      initobj [mscorlib]System.Threading.CancellationToken
      ldloc.s cancellation
      call class [System.IO.Compression]System.IO.Compression.ZipArchive MeloongCore.FileUtils::OpenZip(string, valuetype [mscorlib]System.Threading.CancellationToken)
      stloc.1
'@
    for ($index = 0; $index -lt $markers.Length; ++$index) {
        $body += @"

      ldloc.1
      ldstr "$($markers[$index])"
      callvirt instance class [System.IO.Compression]System.IO.Compression.ZipArchiveEntry [System.IO.Compression]System.IO.Compression.ZipArchive::GetEntry(string)
      brfalse next$index
      ldc.i4 $($values[$index])
      stloc.3
      br done
    next$index : nop
"@
    }
    $body += @'

    done: ldstr "为遵守相关法律法规，PCL 暂时不再提供 MC 下载功能，非常抱歉 qwq……"
      newobj instance void [mscorlib]System.Exception::.ctor(string)
      throw
    } finally {
      ldloc.1
      brfalse disposed
      ldloc.1
      callvirt instance void [System.IO.Compression]System.IO.Compression.ZipArchive::Dispose()
    disposed: endfinally
    }
  } // end of method ModModpack::ModpackInstall
'@
    $text = Replace-Method $text 'ModpackInstall(string' $body
    [IO.File]::WriteAllText($ilFile, $text, [Text.UTF8Encoding]::new($true))
    $fixture = Join-Path $directory 'restricted.exe'
    Invoke-Checked $ilasm @($ilFile, '/exe', '/quiet', "/output:$fixture")
    Invoke-Checked $verify @($fixture, '/nologo', '/md', '/il')
    $patchedDirectory = Join-Path $directory 'patched'
    Invoke-Checked $regression @($patchedDirectory, $fixture)
    $patched = Join-Path $patchedDirectory '1/PCL test.exe'
    Invoke-Checked $verify @($patched, '/nologo', '/md', '/il')
    Invoke-Checked $patched @((Join-Path $directory 'packages'))
}
Write-Output 'Recovery regression and executable behavior checks passed for both metadata layouts.'
