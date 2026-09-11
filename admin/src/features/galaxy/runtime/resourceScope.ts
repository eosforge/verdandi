/** 按注册的相反顺序释放资源; 某项释放失败也继续清理其余项. */
export class ResourceScope {
  private cleanups: Array<() => void> = [];
  private disposed = false;

  // 注册已取得资源的清理动作; 已关闭作用域立即执行新注册项.
  defer(cleanup: () => void): void {
    if (this.disposed) cleanup();
    else this.cleanups.push(cleanup);
  }

  // 接管支持 dispose 的资源, 原样返回供调用者使用.
  own<T extends { dispose(): void }>(resource: T): T {
    this.defer(() => resource.dispose());
    return resource;
  }

  // 幂等关闭并收集异常; 调用者负责报告, 清理过程不抛出以免覆盖初始化错误.
  dispose(): unknown[] {
    if (this.disposed) return [];
    this.disposed = true;
    const failures: unknown[] = [];
    for (const cleanup of this.cleanups.reverse()) {
      try {
        cleanup();
      } catch (error) {
        failures.push(error);
      }
    }
    this.cleanups = [];
    return failures;
  }
}
