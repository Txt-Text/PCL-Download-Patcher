// 可执行行为样本：不启动 PCL、不连接网络，用真实 ZIP 和可观察回调验证恢复后的行为。
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Threading;

#if REORDER
[assembly: AssemblyVersion("2.99.0.0")]
namespace Padding { public class Earlier { public static string Text = "metadata order changed"; public void Unrelated() {} } }
#else
[assembly: AssemblyVersion("2.99.0.0")]
#endif

namespace System.Windows {
    public enum Visibility { Visible, Hidden, Collapsed }
    public class UIElement { public Visibility Visibility { get; set; } }
}
namespace System.Windows.Input { public class MouseButtonEventArgs : EventArgs {} }

namespace MeloongCore {
    public struct OneOf<TFirst, TSecond> {
        public object Value;
        public static implicit operator OneOf<TFirst, TSecond>(TFirst value) { return new OneOf<TFirst, TSecond> { Value = value }; }
        public static implicit operator OneOf<TFirst, TSecond>(TSecond value) { return new OneOf<TFirst, TSecond> { Value = value }; }
    }
    public static class FileUtils {
        public static ZipArchive OpenZip(string file, CancellationToken cancellation) { return ZipFile.OpenRead(file); }
    }
}
namespace MeloongCore.Wpf {
    public static class Dialogs {
        public static string Selected;
        public static List<string> SelectFile(string title, bool multiple, string initial,
            Nullable<MeloongCore.OneOf<IEnumerable<ValueTuple<string[], string>>, string>> filter) {
            if (title != "选择整合包压缩文件" || multiple || initial != null || !filter.HasValue) throw new Exception("picker arguments changed");
            var entries = (IEnumerable<ValueTuple<string[], string>>)filter.Value.Value;
            var formats = entries.Single();
            if (!formats.Item1.SequenceEqual(new[] { "rar", "zip", "mrpack" }) || formats.Item2 != "整合包文件") throw new Exception("picker filter lost");
            return Selected == null ? new List<string>() : new List<string> { Selected };
        }
    }
}

namespace PCL {
    public static class ModLoader {
        public class LoaderCombo<T> { public string Result; }
        public class LoaderTask<TInput, TOutput> {
            private readonly Action<LoaderTask<TInput, TOutput>> action;
            public LoaderTask(string name, Action<LoaderTask<TInput, TOutput>> callback, object ignored, int priority) { action = callback; }
            public void Run() { action(this); }
        }
    }
    public static class ModDownload {
        public struct DlClientListResult {}
        public static bool Loaded;
        public static readonly ModLoader.LoaderTask<string, DlClientListResult> DlClientListLoader =
            new ModLoader.LoaderTask<string, DlClientListResult>("normal", delegate { Loaded = true; }, null, 2);
    }
    public class MyPageRight {
        public void PageLoaderInit<TInput, TOutput>(ModLoader.LoaderTask<TInput, TOutput> loader) { loader.Run(); }
    }
    public class PageDownloadInstall : MyPageRight {
        public System.Windows.UIElement CardMinecraft { get; private set; }
        public PageDownloadInstall() { CardMinecraft = new System.Windows.UIElement(); }
        public void LoaderInit() {
            PageLoaderInit(new ModLoader.LoaderTask<string, ModDownload.DlClientListResult>("关闭下载", delegate { throw new Exception("restricted"); }, null, 2));
        }
    }
    public class PageDownloadModify : PageDownloadInstall {
        public System.Windows.UIElement Unrelated = new System.Windows.UIElement();
        public void LoadInstanceFinished() { CardMinecraft.Visibility = System.Windows.Visibility.Collapsed; Unrelated.Visibility = System.Windows.Visibility.Collapsed; }
    }
    public static class ModMain {
        public enum HintType { Normal, Other, Red }
        public static void Hint(string text, HintType type, bool enabled) { throw new Exception("restricted hint executed"); }
    }
    public static class ModBase {
        public static int ThreadCalls;
        public static void RunInThread(Action callback) { ++ThreadCalls; callback(); }
    }
    public static class ModModpack {
        public static string LastInstall;
        public class EntryClosure {
            public string file;
            public void Execute() { ModpackInstall(file, null, null); }
        }
        public static void ModpackInstall() { GC.KeepAlive(new EntryClosure()); }
        public static ModLoader.LoaderCombo<string> ModpackInstall(string file, string instance, string logo) { throw new Exception("replaced by restricted IL fixture"); }
        // 维持正常 API 所需的开放泛型签名引用；不会被测试入口调用。
        public static void ApiReferences() {
            var tuple = new ValueTuple<string[], string>(new[] { "zip" }, "test");
            MeloongCore.OneOf<IEnumerable<ValueTuple<string[], string>>, string> options = new[] { tuple };
            var nullable = new Nullable<MeloongCore.OneOf<IEnumerable<ValueTuple<string[], string>>, string>>(options);
            var files = MeloongCore.Wpf.Dialogs.SelectFile("unused", false, null, nullable);
            GC.KeepAlive(string.IsNullOrEmpty(files.FirstOrDefault()));
            ModBase.RunInThread(delegate {});
        }
        private static ModLoader.LoaderCombo<string> Record(string kind, ZipArchive zip, string prefix, string instance, string logo) {
            GC.KeepAlive(zip.Entries.Count); // 安装器调用期间归档必须仍然打开。
            LastInstall = kind + ":" + prefix + ":" + instance + ":" + logo;
            return new ModLoader.LoaderCombo<string> { Result = LastInstall };
        }
        public static ModLoader.LoaderCombo<string> InstallPackCurseForge(string file, ZipArchive zip, string prefix, string instance, string logo) { return Record("CurseForge", zip, prefix, instance, logo); }
        public static ModLoader.LoaderCombo<string> InstallPackHMCL(string file, ZipArchive zip, string prefix) { return Record("HMCL", zip, prefix, null, null); }
        public static ModLoader.LoaderCombo<string> InstallPackMMC(string file, ZipArchive zip, string prefix) { return Record("MMC", zip, prefix, null, null); }
        public static ModLoader.LoaderCombo<string> InstallPackMCBBS(string file, ZipArchive zip, string prefix, string instance) { return Record("MCBBS", zip, prefix, instance, null); }
        public static ModLoader.LoaderCombo<string> InstallPackModrinth(string file, ZipArchive zip, string prefix, string instance, string logo) { return Record("Modrinth", zip, prefix, instance, logo); }
        public static ModLoader.LoaderCombo<string> InstallPackLauncherPack(string file, ZipArchive zip, string prefix) { return Record("Launcher", zip, prefix, null, null); }
        public static ModLoader.LoaderCombo<string> InstallPackCompress(string file, ZipArchive zip) { return Record("Compress", zip, null, null, null); }
    }
    public class MyListItem { public delegate void ClickEventHandler(object sender, System.Windows.Input.MouseButtonEventArgs args); }
    public class MyIconButton { public delegate void ClickEventHandler(object sender, EventArgs args); }
    public class MyVirtualizingElement<T> {
        public MyListItem.ClickEventHandler Click;
        public MyIconButton.ClickEventHandler Save;
    }
    public class ResourceVersion {
        public MyVirtualizingElement<MyListItem> ToListItem(MyListItem.ClickEventHandler click, MyIconButton.ClickEventHandler save, bool duplicates) {
            return new MyVirtualizingElement<MyListItem> { Click = click, Save = save };
        }
    }
    public enum ResourceTypes {
        Mod = 1,
#if REORDER
        ModPack = 37 // 特意改变枚举编号，证明规则不是按某个版本的编号硬编码。
#else
        ModPack = 2
#endif
    }
    public class PageDownloadResourceDetail {
        public ResourceTypes TargetResourceType;
        public int Installs, Saves;
        public void Install_Click(MyListItem sender, EventArgs args) { ++Installs; }
        public void Save_Click(object sender, EventArgs args) { ++Saves; }
        public void InstallAdapter(object sender, System.Windows.Input.MouseButtonEventArgs args) { Install_Click((MyListItem)sender, args); }
        private static bool Duplicates() { return Environment.TickCount < 0; }
        public void InitializeStackResourceVersions(List<MyVirtualizingElement<MyListItem>> stack) {
            bool duplicates = Duplicates();
            foreach (var version in new[] { new ResourceVersion() }) { stack.Add(version.ToListItem(Save_Click, null, duplicates)); }
        }
    }
    public static class RecoveryProgram {
        private static void Check(bool value, string description) { if (!value) throw new Exception(description); }
        public static void Main(string[] arguments) {
            try {
                string root = arguments[0]; Directory.CreateDirectory(root);
                new PageDownloadInstall().LoaderInit(); Check(ModDownload.Loaded, "download loader");
                var modify = new PageDownloadModify(); modify.LoadInstanceFinished();
                Check(modify.CardMinecraft.Visibility == System.Windows.Visibility.Visible && modify.Unrelated.Visibility == System.Windows.Visibility.Collapsed, "visibility isolation");
                var resources = new PageDownloadResourceDetail(); var list = new List<MyVirtualizingElement<MyListItem>>();
                resources.TargetResourceType = ResourceTypes.ModPack; resources.InitializeStackResourceVersions(list);
                list[0].Click(new MyListItem(), new System.Windows.Input.MouseButtonEventArgs()); Check(resources.Installs == 1 && resources.Saves == 0, "modpack click");
                Check(list[0].Save != null, "modpack save button missing"); list[0].Save(new MyIconButton(), EventArgs.Empty); Check(resources.Saves == 1, "modpack save click");
                list.Clear(); resources.TargetResourceType = ResourceTypes.Mod; resources.InitializeStackResourceVersions(list);
                list[0].Click(new MyListItem(), new System.Windows.Input.MouseButtonEventArgs()); Check(resources.Installs == 1 && resources.Saves == 2 && list[0].Save == null, "ordinary resource changed to install");
                string[] markers = { "manifest.json", "modpack.json", "mmc-pack.json", "mcbbs.packmeta", "modrinth.index.json", "modpack.zip", "unknown.txt" };
                string[] expected = { "CurseForge::instance:logo", "HMCL:::", "MMC:::", "MCBBS::instance:", "Modrinth::instance:logo", "Launcher:::", "Compress:::" };
                string firstFile = null;
                for (int index = 0; index < markers.Length; ++index) {
                    string file = Path.Combine(root, index + ".zip"); if (index == 0) firstFile = file;
                    using (var zip = ZipFile.Open(file, ZipArchiveMode.Create)) { zip.CreateEntry(markers[index]); }
                    var installed = ModModpack.ModpackInstall(file, "instance", "logo"); Check(installed.Result == expected[index], "format dispatch " + markers[index]);
                    using (File.Open(file, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) {} // finally 必须已经释放归档。
                }
                MeloongCore.Wpf.Dialogs.Selected = null; ModModpack.ModpackInstall(); Check(ModBase.ThreadCalls == 0, "picker cancellation started install");
                MeloongCore.Wpf.Dialogs.Selected = firstFile; ModModpack.ModpackInstall(); Check(ModBase.ThreadCalls == 1 && ModModpack.LastInstall == "CurseForge:::", "picker background dispatch");
                Console.WriteLine("BEHAVIOR PASS: loader, visibility, resource install/save, seven formats, archive disposal, picker/cancel");
            } catch (Exception error) { Console.Error.WriteLine(error); Environment.ExitCode = 1; }
        }
    }
}
