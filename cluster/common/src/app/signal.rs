//! 只在可执行程序中安装操作系统信号, 不改变库使用者的进程级信号行为.

use std::io;

/// 在绑定 listener 前安装监听, 防止启动完成与信号注册之间存在未保护窗口.
pub struct Shutdown {
    /// Unix 的 SIGINT 监听, 与终端 Ctrl+C 对应.
    #[cfg(unix)]
    interrupt: tokio::signal::unix::Signal,
    /// Unix 进程管理器发出的 SIGTERM 停止请求.
    #[cfg(unix)]
    terminate: tokio::signal::unix::Signal,
    /// Windows 控制台 Ctrl+C 监听.
    #[cfg(windows)]
    interrupt: tokio::signal::windows::CtrlC,
    /// Windows 控制台 Ctrl+Break 监听, 不承担 SCM 服务控制协议.
    #[cfg(windows)]
    terminate: tokio::signal::windows::CtrlBreak,
}

impl Shutdown {
    /// 注册失败直接返回 I/O 错误, 尚未启动任何网络任务.
    pub fn new() -> io::Result<Self> {
        #[cfg(unix)]
        {
            use tokio::signal::unix::{SignalKind, signal};
            Ok(Self {
                interrupt: signal(SignalKind::interrupt())?,
                terminate: signal(SignalKind::terminate())?,
            })
        }
        #[cfg(windows)]
        {
            Ok(Self {
                interrupt: tokio::signal::windows::ctrl_c()?,
                terminate: tokio::signal::windows::ctrl_break()?,
            })
        }
    }

    /// 等待一次停止请求. 信号流异常关闭必须作为失败报告, 不能悄悄永久等待.
    pub async fn wait(&mut self) -> io::Result<()> {
        tokio::select! {
            value = self.interrupt.recv() => value,
            value = self.terminate.recv() => value,
        }
        .ok_or_else(|| io::Error::other("shutdown signal stream closed"))
    }
}
