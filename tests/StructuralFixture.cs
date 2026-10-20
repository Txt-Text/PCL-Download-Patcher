// 合成程序集仅用于测试元数据令牌和方法位置变化，不是 PCL 版本替身。
using System;
using System.Reflection;

#if REORDER
[assembly: AssemblyVersion("9.8.7.6")]
namespace Padding {
    public class BeforePcl {
        public static string unrelated = "Change metadata tokens and method locations";
        public static void ExtraMethod() { Console.WriteLine(unrelated); }
    }
}
#else
[assembly: AssemblyVersion("1.0.0.0")]
#endif

namespace PCL {
    public static class ModLoader {
        public class LoaderTask<TInput, TOutput> {
            private readonly Action<LoaderTask<TInput, TOutput>> action;
            public LoaderTask(string name, Action<LoaderTask<TInput, TOutput>> action, object ignored, int priority) {
                this.action = action;
            }
            public void Run() { action(this); }
        }
    }

    public static class ModDownload {
        public struct DlClientListResult {}
        public static readonly ModLoader.LoaderTask<string, DlClientListResult> DlClientListLoader =
            new ModLoader.LoaderTask<string, DlClientListResult>("Normal", delegate { Console.WriteLine("DOWNLOAD_LOADER"); }, null, 2);
    }

    public class MyPageRight {
        public void PageLoaderInit<TInput, TOutput>(ModLoader.LoaderTask<TInput, TOutput> task) { task.Run(); }
    }

    public class PageDownloadInstall : MyPageRight {
        public void LoaderInit() {
            PageLoaderInit(new ModLoader.LoaderTask<string, ModDownload.DlClientListResult>("关闭下载", delegate { throw new InvalidOperationException("DOWNLOAD_BLOCKED"); }, null, 2));
        }
    }

    public static class Program {
        public static void Main() {
            try { new PageDownloadInstall().LoaderInit(); }
            catch (InvalidOperationException error) { Console.WriteLine(error.Message); }
        }
    }
}
