export interface FrameScheduler {
  request(callback: (time: number) => void): number;
  cancel(id: number): void;
}

/** 唯一 RAF 所有者; 隐藏或失去上下文时由场景显式暂停. */
export class FrameLoop {
  private frameId: number | null = null;
  private active = false;
  private disposed = false;
  private lastDraw: number | null = null;
  private nextDraw: number | null = null;
  private fpsStart: number | null = null;
  private frames = 0;
  private readonly interval: number;
  private readonly scheduler: FrameScheduler;
  private readonly render: (deltaSeconds: number) => void;
  private readonly onFps: (fps: number) => void;
  private readonly onError: (error: unknown) => void;

  // 注入时钟调度以便验证丢帧和后台恢复; render 异常交给 onError 并停止循环.
  constructor(
    scheduler: FrameScheduler,
    render: (deltaSeconds: number) => void,
    onFps: (fps: number) => void,
    onError: (error: unknown) => void,
    targetFps = 60,
  ) {
    if (!Number.isFinite(targetFps) || targetFps <= 0) throw new RangeError("Invalid target FPS");
    this.interval = 1000 / targetFps;
    this.scheduler = scheduler;
    this.render = render;
    this.onFps = onFps;
    this.onError = onError;
  }

  // 多次启动只保留一个请求; 恢复后的第一帧不补算暂停时间.
  start(): void {
    if (this.disposed || this.active) return;
    this.active = true;
    this.frameId = this.scheduler.request(this.tick);
  }

  // 取消唯一请求并重置采样窗口, 避免隐藏时间进入公转和 FPS.
  stop(): void {
    this.active = false;
    if (this.frameId !== null) this.scheduler.cancel(this.frameId);
    this.frameId = null;
    this.lastDraw = null;
    this.nextDraw = null;
    this.fpsStart = null;
    this.frames = 0;
  }

  // 永久停止, 此后 start 无效.
  dispose(): void {
    this.stop();
    this.disposed = true;
  }

  // 按累计截止时间限帧, 不以取整跳帧把 60 Hz 显示器降成 30 FPS.
  private tick = (now: number): void => {
    this.frameId = null;
    if (!this.active) return;
    this.nextDraw ??= now;
    if (now + 0.75 >= this.nextDraw) {
      this.nextDraw += Math.max(1, Math.floor((now - this.nextDraw) / this.interval) + 1) * this.interval;
      const delta = this.lastDraw === null ? 0 : Math.max(0, Math.min((now - this.lastDraw) / 1000, 0.1));
      this.lastDraw = now;
      try {
        this.render(delta);
        if (this.fpsStart === null) this.fpsStart = now;
        else {
          this.frames++;
          const elapsed = now - this.fpsStart;
          if (elapsed >= 1000) {
            this.onFps(Math.round((this.frames * 1000) / elapsed));
            this.fpsStart = now;
            this.frames = 0;
          }
        }
      } catch (error) {
        this.stop();
        this.onError(error);
        return;
      }
    }
    if (this.active) this.frameId = this.scheduler.request(this.tick);
  };
}
