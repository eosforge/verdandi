//! Planet 可执行入口, 与 Star 共用参数解析和运行基础.
mod app;
fn main() -> std::process::ExitCode {
    app::main()
}
