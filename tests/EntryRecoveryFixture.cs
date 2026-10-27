// 新增入口的可执行样本：仅测试内存中的控件和设置，不打开 PCL 或连接网络。
using System;
using System.Collections.Generic;
using System.Reflection;
[assembly: AssemblyVersion("2.99.0.1")]

namespace System.Windows {
    public enum Visibility { Visible, Hidden, Collapsed }
    public class UIElement { public Visibility Visibility { get; set; } }
    namespace Controls {
        public class TextBlock : UIElement {}
        public class UIElementCollection : List<UIElement> { public new int Add(UIElement item) { base.Add(item); return Count - 1; } }
        public class Panel : UIElement { public UIElementCollection Children { get; private set; } public Panel() { Children = new UIElementCollection(); } }
        public class StackPanel : Panel {}
    }
}
namespace PCL {
    public class MyButton : System.Windows.UIElement {}
    public class MyCard { public void TriggerForceResize() {} }
    public class MyListItem : System.Windows.UIElement {
        public string Title { get; set; }
        public bool Checked { get; set; }
        public event Action Click;
        public void Invoke() { if (Click != null) Click(); }
    }
    public class MyPageLeft {}
    public static class FormMain { public enum PageSubType { DownloadInstall, Other, DownloadMod } }
    public static class PageSetupUI { public static bool HiddenForceShow { get; set; } }
    public static class Settings {
        public static bool Hide;
        public static bool Seed() { return Get<bool>("UiHiddenPageDownload", null); }
        public static T Get<T>(string key, ModMinecraft.McInstance instance) { return (T)(object)Hide; }
    }
    public static class ModMain {
        public static PageSelectLeft FrmSelectLeft;
        public static PageSelectRight FrmSelectRight;
        public static string UpdateButton;
        public static int MyMsgBox(string title, string ok, string download) { UpdateButton = download; return 2; }
    }
    public static class ModModpack { public static int Calls; public static void ModpackInstall() { ++Calls; } }
    public static class ModMinecraft {
        public class McInstance {}
        public static McInstance McInstanceSelected { get; set; }
        public static void McDownloadClientUpdateHint() { if (ModMain.MyMsgBox("Minecraft 更新提示", "确定", "") == 2) GC.KeepAlive("download-handler"); }
    }
    public class PageDownloadLeft : MyPageLeft {
        private MyListItem install, mod;
        private System.Windows.Controls.TextBlock resource;
        public MyListItem ItemInstall { get { return install; } }
        public MyListItem ItemMod { get { return mod; } }
        public System.Windows.Controls.TextBlock LabResource { get { return resource; } }
        public FormMain.PageSubType PageID;
        public void InitializeComponent() { install = new MyListItem(); mod = new MyListItem(); resource = new System.Windows.Controls.TextBlock(); }
        public PageDownloadLeft() { InitializeComponent(); } // 脚本替换为与目标相同的拦截构造函数。
    }
    public enum ResourceTypes { Mod, Other, ModPack }
    public class PageResource {
        private ResourceTypes _Type;
        public MyButton BtnSearchInstallModPack { get; private set; }
        public PageResource() { BtnSearchInstallModPack = new MyButton(); }
        public ResourceTypes PageType { get { return _Type; } set { _Type = value; } } // 脚本加入拦截。
    }
    public class PageInstanceModDisabled {
        public MyButton BtnDownload { get; private set; }
        public MyCard PanMain { get; private set; }
        public PageInstanceModDisabled() { BtnDownload = new MyButton(); PanMain = new MyCard(); }
        public void BtnDownload_Loaded() {} // 脚本加入拦截。
    }
    public class PageLaunchLeft {
        public int BtnLaunchState;
        public void RefreshButtonsUI() {} // 脚本加入拦截。
    }
    public class PageSelectRight {
        public bool ShowHidden;
        public bool Empty;
        public MyButton BtnEmptyDownload { get; private set; }
        public PageSelectRight() { BtnEmptyDownload = new MyButton(); }
        public void McInstanceListUI() {} // 脚本加入两种空列表状态。
    }
    public class PageSelectLeft {
        public System.Windows.Controls.StackPanel PanList { get; private set; }
        public PageSelectLeft() { PanList = new System.Windows.Controls.StackPanel(); }
        private void Install() { ModModpack.ModpackInstall(); }
        public void McFolderListUI() {} // 脚本加入未显示的整合包选项。
    }
    public static class EntryRecoveryFixture {
        private static void Require(bool value, string message) { if (!value) throw new Exception(message); }
        public static void Main() {
            var left = new PageDownloadLeft();
            Require(left.ItemInstall.Visibility == System.Windows.Visibility.Visible && left.LabResource.Visibility == System.Windows.Visibility.Visible &&
                left.PageID == FormMain.PageSubType.DownloadInstall && !left.ItemMod.Checked, "download navigation");
            var resource = new PageResource(); resource.PageType = ResourceTypes.ModPack;
            Require(resource.BtnSearchInstallModPack.Visibility == System.Windows.Visibility.Visible, "modpack search button");
            resource.PageType = ResourceTypes.Mod;
            Require(resource.BtnSearchInstallModPack.Visibility == System.Windows.Visibility.Collapsed, "ordinary resource button");
            foreach (bool hide in new[] { false, true }) foreach (bool force in new[] { false, true }) {
                Settings.Hide = hide; PageSetupUI.HiddenForceShow = force;
                bool hidden = hide && !force;
                var select = new PageSelectRight(); ModMain.FrmSelectRight = select;
                select.Empty = true; select.McInstanceListUI();
                Require(select.BtnEmptyDownload.Visibility == (hidden ? System.Windows.Visibility.Collapsed : System.Windows.Visibility.Visible), "empty list settings");
                select.Empty = false; select.McInstanceListUI();
                Require(select.BtnEmptyDownload.Visibility == System.Windows.Visibility.Collapsed, "hidden instance list");
                var launch = new PageLaunchLeft(); ModMinecraft.McInstanceSelected = null; launch.RefreshButtonsUI();
                Require(launch.BtnLaunchState == (hidden ? 1 : 2), "launch download state");
                ModMinecraft.McInstanceSelected = new ModMinecraft.McInstance(); launch.RefreshButtonsUI();
                Require(launch.BtnLaunchState == 3, "selected instance launch state");
                foreach (bool show in new[] { false, true }) {
                    select.ShowHidden = show; var disabled = new PageInstanceModDisabled(); disabled.BtnDownload_Loaded();
                    Require(disabled.BtnDownload.Visibility == (hidden || show ? System.Windows.Visibility.Collapsed : System.Windows.Visibility.Visible), "disabled mod settings");
                }
                ModMain.FrmSelectRight = null; var absent = new PageInstanceModDisabled(); absent.BtnDownload_Loaded();
                Require(absent.BtnDownload.Visibility == (hidden ? System.Windows.Visibility.Collapsed : System.Windows.Visibility.Visible), "absent select page");
            }
            ModMinecraft.McDownloadClientUpdateHint(); Require(ModMain.UpdateButton == "下载", "update button");
            ModMain.FrmSelectLeft = new PageSelectLeft(); ModMain.FrmSelectLeft.McFolderListUI();
            Require(ModMain.FrmSelectLeft.PanList.Children.Count == 1, "import entry missing or duplicated");
            ((MyListItem)ModMain.FrmSelectLeft.PanList.Children[0]).Invoke(); Require(ModModpack.Calls == 1, "import handler");
            Console.WriteLine("Entry behavior PASS: seven entries and all hide/force/show settings combinations.");
        }
    }
}
