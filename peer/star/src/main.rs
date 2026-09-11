//! Peer 可执行程序入口. 进程组合位于 app, 网络库不依赖此模块.

mod app;

/// 返回 app 已完成清理后的进程退出码.
fn main() -> std::process::ExitCode {
    app::main()
}
