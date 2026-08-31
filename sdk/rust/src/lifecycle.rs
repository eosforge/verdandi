use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

use tokio::sync::{Mutex, Notify};
use tokio_util::sync::CancellationToken;

use crate::{Code, Error, Result};

/// 合并领域 Client 共用的关闭准入、活动计数和公开句柄计数。
///
/// Activity 只在 crate 内可见，不改变公开 API；具体领域仍然拥有自己的策略、资源和关闭副作用。
pub(crate) struct Activity {
    shutdown: CancellationToken,
    closed: AtomicBool,
    active: AtomicUsize,
    idle: Notify,
    close_gate: Mutex<()>,
    close_finished: AtomicBool,
    handles: AtomicUsize,
}

/// Guard 表示一项已通过领域关闭栅栏的活动工作。
///
/// Drop 是唯一释放路径，保证错误返回、取消和 panic 展开都不会泄漏活动计数。
pub(crate) struct Guard {
    owner: Arc<Activity>,
}

impl Activity {
    /// 使用领域取消令牌创建一个拥有单个公开句柄的活动栅栏。
    pub(crate) fn new(shutdown: CancellationToken) -> Self {
        Self {
            shutdown,
            closed: AtomicBool::new(false),
            active: AtomicUsize::new(0),
            idle: Notify::new(),
            close_gate: Mutex::new(()),
            close_finished: AtomicBool::new(false),
            handles: AtomicUsize::new(1),
        }
    }

    /// 返回领域显式关闭或根级取消是否已经发生。
    pub(crate) fn is_closed(&self) -> bool {
        self.closed.load(Ordering::Acquire) || self.shutdown.is_cancelled()
    }

    /// 原子封住新准入，并且只在第一次调用时取消领域令牌。
    pub(crate) fn start_shutdown(&self) {
        if !self.closed.swap(true, Ordering::AcqRel) {
            self.shutdown.cancel();
        }
    }

    /// 串行等待全部 Guard 释放，并让并发 Close 复用同一完成状态。
    pub(crate) async fn finish_close(&self) {
        let _gate = self.close_gate.lock().await;
        if self.close_finished.load(Ordering::Acquire) {
            return;
        }
        // 先启用 Notify waiter 再复查活动数，避免最后一个 Guard 在注册等待者前释放而丢失唤醒。
        while self.active.load(Ordering::Acquire) != 0 {
            let notified = self.idle.notified();
            tokio::pin!(notified);
            notified.as_mut().enable();
            if self.active.load(Ordering::Acquire) == 0 {
                break;
            }
            notified.await;
        }
        self.close_finished.store(true, Ordering::Release);
    }

    /// 无锁接纳一项领域工作，并用关闭后的第二次检查回滚竞态准入。
    pub(crate) fn admit(self: &Arc<Self>) -> Result<Guard> {
        if self.is_closed() {
            return Err(Error::new(Code::Closed));
        }
        self.active.fetch_add(1, Ordering::AcqRel);
        if self.is_closed() {
            self.release();
            return Err(Error::new(Code::Closed));
        }
        Ok(Guard { owner: Arc::clone(self) })
    }

    /// 为一个新公开 Client 克隆句柄增加所有者计数。
    pub(crate) fn add_handle(&self) {
        self.handles.fetch_add(1, Ordering::Relaxed);
    }

    /// 释放一个公开 Client 句柄，并返回它是否为最后一个所有者。
    pub(crate) fn drop_handle(&self) -> bool {
        self.handles.fetch_sub(1, Ordering::AcqRel) == 1
    }

    /// 释放一个活动计数，并在归零时唤醒所有关闭等待者。
    fn release(&self) {
        if self.active.fetch_sub(1, Ordering::AcqRel) == 1 {
            self.idle.notify_waiters();
        }
    }
}

impl Drop for Guard {
    fn drop(&mut self) {
        self.owner.release();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn shutdown_rejects_new_work_and_waits_for_existing_guards() {
        let activity = Arc::new(Activity::new(CancellationToken::new()));
        let guard = match activity.admit() {
            Ok(guard) => guard,
            Err(error) => panic!("initial admission failed: {error}"),
        };
        activity.start_shutdown();
        assert!(matches!(activity.admit(), Err(error) if error.code() == Code::Closed));

        let waiter = {
            let activity = Arc::clone(&activity);
            tokio::spawn(async move { activity.finish_close().await })
        };
        tokio::task::yield_now().await;
        assert!(!waiter.is_finished());
        drop(guard);
        if let Err(error) = waiter.await {
            panic!("close waiter failed: {error}");
        }
        activity.finish_close().await;
    }
}
