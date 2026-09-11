//! 两种可执行入口共用参数, 信号和日志, 库节点本身不安装进程信号.
pub mod cli;
pub mod logging;
mod process;
pub mod signal;
pub use process::launch;
