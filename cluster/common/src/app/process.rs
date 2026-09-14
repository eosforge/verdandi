//! 两种可执行程序共用参数处理和 runtime 所有权, 角色层只提供其运行函数.
use super::cli;
use std::{
    env,
    io::{self, Write},
    process::ExitCode,
};

/// 运行一个角色入口. component/version 只用于帮助与诊断; run 借用 stdout, 不将其分离到后台.
/// AsyncFnOnce 让不同角色的 async fn 静态调度, 无需 trait 对象, 装箱 future 或通用节点接口.
/// 帮助/正常退出为 0, 参数错误为 2, I/O/runtime/角色错误为 1. 不修改全局环境.
pub fn launch(component: &str, version: &str, run: impl AsyncFnOnce(cli::Options, &mut io::Stdout) -> io::Result<()>) -> ExitCode {
    let args = match env::args_os().skip(1).map(|arg| arg.into_string()).collect::<Result<Vec<_>, _>>() {
        Ok(args) => args,
        Err(_) => {
            let _ = writeln!(io::stderr(), "{component}: arguments must be valid Unicode");
            return ExitCode::from(2);
        }
    };
    let mut stdout = io::stdout();
    if args == ["--version"] {
        return output_result(component, writeln!(stdout, "{component} {version}"));
    }
    let options = match cli::Options::parse(args) {
        Ok(Some(options)) => options,
        Ok(None) => {
            return output_result(
                component,
                stdout.write_all(cli::HELP.replace("Usage: peer", &format!("Usage: {component}")).as_bytes()),
            );
        }
        Err(error) => {
            let _ = writeln!(io::stderr(), "{component}: {error}");
            return ExitCode::from(2);
        }
    };
    // 参数全部验证后才创建有明确线程上限的 runtime, 整个异步角色运行期间独占此 runtime.
    let runtime = match tokio::runtime::Builder::new_multi_thread().worker_threads(options.workers).enable_all().build() {
        Ok(runtime) => runtime,
        Err(error) => return output_result(component, Err(error)),
    };
    output_result(component, runtime.block_on(run(options, &mut stdout)))
}

/// writer 或 runtime 错误走同一失败出口, 诊断输出再次失败时也不 panic.
fn output_result(component: &str, result: io::Result<()>) -> ExitCode {
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            let _ = writeln!(io::stderr(), "{component}: {error}");
            ExitCode::FAILURE
        }
    }
}
