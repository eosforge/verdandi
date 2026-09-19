// 场景展示与交互参数; 距离均为模型世界单位, 时间均为真实秒, 不描述后端业务规则.
export const sceneConfig = {
  // 不透明画布背景的 sRGB 十六进制颜色.
  background: 0x050914,
  // 帧调度目标上限; 实际 FPS 仍受浏览器刷新率与 GPU 影响.
  targetFps: 60,
  // 绘图缓冲的设备像素比上限, 正数; 实际值取设备 DPR 与此值的较小者.
  maxPixelRatio: 1.5,
  // 黑洞整体线性缩放; 盘面、阴影、拾取与轨道障碍半径使用同一个比例.
  blackHoleScale: 0.75,
  // 公转虚拟时间倍率; 选中行星时统一暂停, 不允许单独改变一颗行星的时间.
  orbitSpeed: {
    // 总览时加快整体公转.
    overview: 5,
    // 聚焦恒星后减速, 便于选择行星.
    star: 2,
  },
  // 可用恒星统一以一分钟一圈自转, 单位弧度/秒; 调用方可按节点覆盖或设为零停转.
  starRotationRadiansPerSecond: (Math.PI * 2) / 60,
  camera: {
    // 垂直视场角, 单位度, 必须处于 0..180 之间.
    fieldOfView: 45,
    // 近裁剪距离, 正数且小于 far.
    near: 0.1,
    // 远裁剪距离, 大于相机最大观察距离与星图范围.
    far: 1600,
    // 手动缩放允许的最近观察距离, 防止进入星体内部.
    minDistance: 8,
    // 手动缩放和恒星聚焦的最远观察距离.
    maxDistance: 440,
    // 恒星聚焦的最小距离; 大星系按轨道范围自动后退.
    starDistance: 90,
    // 行星聚焦从当前行星位置向外偏移的距离.
    planetDistance: 18,
    // 聚焦或返回总览的平滑过渡时长, 正数.
    transitionSeconds: 1.2,
    // 滚轮缩放指数收敛的时间常数, 正数.
    zoomResponseSeconds: 0.09,
    // 拖动响应时间常数为 90 ms; 旋转和平移倍率适当降低, 便于近景细调.
    dragResponseSeconds: 0.09,
    // 左键旋转相对于 OrbitControls 默认值的正倍率.
    rotateSpeed: 0.65,
    // 中键平移相对于 OrbitControls 默认值的正倍率.
    panSpeed: 0.8,
    // 初始及返回总览的世界坐标 [x, y, z].
    overviewPosition: [0, 112, 205],
    // 总览相机观察中心的世界坐标 [x, y, z].
    overviewTarget: [0, 0, 16],
  },
} as const;
