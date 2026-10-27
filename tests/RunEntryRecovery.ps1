# 独立生成合成程序并实际执行修补后的入口；从不修改历史 PCL 文件。
param([string]$OutputDirectory = (Join-Path $PSScriptRoot ('output/entries-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))))
$ErrorActionPreference = 'Stop'
$csc = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$ilasm = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/ilasm.exe'
$ildasm = Join-Path ${env:ProgramFiles(x86)} 'Microsoft SDKs/Windows/v10.0A/bin/NETFX 4.8 Tools/ildasm.exe'
$verify = Join-Path (Split-Path $ildasm) 'PEVerify.exe'
if (Test-Path -LiteralPath $OutputDirectory) { throw '输出目录已存在，请指定新目录。' }
$directory = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
function Invoke-Checked([string]$Tool, [string[]]$Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed: $LASTEXITCODE" }
}
Invoke-Checked $csc @('/nologo', '/optimize+', "/out:$directory/source.exe", (Join-Path $PSScriptRoot 'EntryRecoveryFixture.cs'))
Invoke-Checked $ildasm @("$directory/source.exe", '/text', '/utf8', '/nobar', "/out:$directory/source.il")
$text = [IO.File]::ReadAllText("$directory/source.il")
function Replace-Method([string]$Name, [string]$Body) {
    $marker = "} // end of method $Name"
    $end = $script:text.IndexOf($marker)
    if ($end -lt 0) { throw "未找到测试函数：$Name" }
    $start = $script:text.LastIndexOf('  .method ', $end)
    $script:text = $script:text.Remove($start, $end + $marker.Length - $start).Insert($start, $Body + "`n  " + $marker)
}
Replace-Method 'PageDownloadLeft::.ctor' @'
  .method public specialname rtspecialname instance void .ctor() cil managed {
    .maxstack 2
    ldarg.0
    call instance void PCL.MyPageLeft::.ctor()
    ldarg.0
    ldc.i4.0
    stfld valuetype PCL.FormMain/PageSubType PCL.PageDownloadLeft::PageID
    ldarg.0
    call instance void PCL.PageDownloadLeft::InitializeComponent()
    ldarg.0
    callvirt instance class PCL.MyListItem PCL.PageDownloadLeft::get_ItemInstall()
    ldc.i4.2
    callvirt instance void System.Windows.UIElement::set_Visibility(valuetype System.Windows.Visibility)
    ldarg.0
    callvirt instance class System.Windows.Controls.TextBlock PCL.PageDownloadLeft::get_LabResource()
    ldc.i4.2
    callvirt instance void System.Windows.UIElement::set_Visibility(valuetype System.Windows.Visibility)
    ldarg.0
    ldc.i4.2
    stfld valuetype PCL.FormMain/PageSubType PCL.PageDownloadLeft::PageID
    ldarg.0
    callvirt instance class PCL.MyListItem PCL.PageDownloadLeft::get_ItemMod()
    ldc.i4.1
    callvirt instance void PCL.MyListItem::set_Checked(bool)
    ret
'@
Replace-Method 'PageResource::set_PageType' @'
  .method public specialname instance void set_PageType(valuetype PCL.ResourceTypes 'value') cil managed {
    .maxstack 4
    ldarg.0
    ldfld valuetype PCL.ResourceTypes PCL.PageResource::_Type
    ldarg.1
    beq done
    ldarg.0
    ldarg.1
    stfld valuetype PCL.ResourceTypes PCL.PageResource::_Type
    ldarg.0
    callvirt instance class PCL.MyButton PCL.PageResource::get_BtnSearchInstallModPack()
    ldarg.1
    ldc.i4.2
    pop
    pop
    ldc.i4.2
    callvirt instance void System.Windows.UIElement::set_Visibility(valuetype System.Windows.Visibility)
  done: ret
'@
Replace-Method 'PageInstanceModDisabled::BtnDownload_Loaded' @'
  .method public instance void BtnDownload_Loaded() cil managed {
    .maxstack 2
    .locals init (valuetype System.Windows.Visibility visibility)
    ldc.i4.2
    stloc.0
    ldarg.0
    callvirt instance class PCL.MyButton PCL.PageInstanceModDisabled::get_BtnDownload()
    ldloc.0
    callvirt instance void System.Windows.UIElement::set_Visibility(valuetype System.Windows.Visibility)
    ret
'@
Replace-Method 'PageLaunchLeft::RefreshButtonsUI' @'
  .method public instance void RefreshButtonsUI() cil managed {
    .maxstack 2
    .locals init (int32 state)
    call class PCL.ModMinecraft/McInstance PCL.ModMinecraft::get_McInstanceSelected()
    brtrue selected
    ldc.i4.1
    stloc.0
    br done
  selected: ldc.i4.3
    stloc.0
  done: ldarg.0
    ldloc.0
    stfld int32 PCL.PageLaunchLeft::BtnLaunchState
    ret
'@
Replace-Method 'PageSelectRight::McInstanceListUI' @'
  .method public instance void McInstanceListUI() cil managed {
    .maxstack 2
    ldarg.0
    ldfld bool PCL.PageSelectRight::Empty
    brtrue empty
    ldarg.0
    callvirt instance class PCL.MyButton PCL.PageSelectRight::get_BtnEmptyDownload()
    ldc.i4.2
    callvirt instance void System.Windows.UIElement::set_Visibility(valuetype System.Windows.Visibility)
    br done
  empty: ldarg.0
    callvirt instance class PCL.MyButton PCL.PageSelectRight::get_BtnEmptyDownload()
    ldc.i4.2
    callvirt instance void System.Windows.UIElement::set_Visibility(valuetype System.Windows.Visibility)
  done: ret
'@
Replace-Method 'PageSelectLeft::McFolderListUI' @'
  .method public instance void McFolderListUI() cil managed {
    .maxstack 3
    .locals init (class PCL.MyListItem item)
    newobj instance void PCL.MyListItem::.ctor()
    dup
    ldstr "导入整合包"
    callvirt instance void PCL.MyListItem::set_Title(string)
    stloc.0
    ldloc.0
    ldarg.0
    ldftn instance void PCL.PageSelectLeft::Install()
    newobj instance void [mscorlib]System.Action::.ctor(object, native int)
    callvirt instance void PCL.MyListItem::add_Click(class [mscorlib]System.Action)
    ret
'@
[IO.File]::WriteAllText("$directory/source.il", $text, [Text.UTF8Encoding]::new($true))
Invoke-Checked $ilasm @("$directory/source.il", '/exe', '/quiet', "/output:$directory/restricted.exe")
Invoke-Checked $verify @("$directory/restricted.exe", '/nologo')
Invoke-Checked (Join-Path $PSScriptRoot 'output/EntryRegression.exe') @("$directory/restricted.exe", "$directory/patched.exe")
Invoke-Checked $verify @("$directory/patched.exe", '/nologo')
Invoke-Checked "$directory/patched.exe" @()
