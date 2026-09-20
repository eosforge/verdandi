// 场景展示与交互参数; 距离均为模型世界单位, 时间均为真实秒, 不描述后端业务规则.
export const sceneConfig = {
  // 不透明画布背景的 sRGB 十六进制颜色.
  background: 0x050914,
  // 背景专用辐射纹理与视觉噪声; 强度为线性颜色增量, 不改变星体材质、曝光或环境光.
  cosmicBackground: {
    // 关闭后恢复纯色深空背景, 无额外背景绘制.
    enabled: true,
    // 低频冷暖起伏的亮度系数, 保持远低于行星和恒星的亮度.
    radiationStrength: 0.014,
    // 固定细粒噪声的峰峰值, 不随时间随机闪烁.
    grainStrength: 0.00015,
    // 整个天球的远景星点数量, 单次实例批次; 可见视野只占其中一小部分.
    distantStarCount: 2200,
    // 远景星点的线性亮度系数, 不参与场景照明.
    distantStarStrength: 0.12,
  },
  // 帧调度目标上限; 实际 FPS 仍受浏览器刷新率与 GPU 影响.
  targetFps: 60,
  // 绘图缓冲的设备像素比上限, 正数; 实际值取设备 DPR 与此值的较小者.
  maxPixelRatio: 1.5,
  // 是否绘制恒星连接边; 关闭只隐藏连线, 保留拓扑数据与详情中的连接数量.
  showStarLinks: false,
  // 普通恒星在原 1.5 倍线性尺寸上体积翻倍; 立方根换算同步作用于表面、日冕和避碰范围.
  starScale: 1.5 * Math.cbrt(2),
  // 脉冲星在行星数量缩放之上的整体线性倍率, 核心、磁场与光束保持一致比例.
  pulsarScale: 8,
  // 点击直径倍率: 普通恒星以表面为基准, 黑洞以含吸积盘的完整光学范围为基准; 不改变显示和布局.
  starPickDiameterScale: 3,
  // 恒星（含黑洞状态）的独立公转时间倍率; 总览与恒星视图均为 15, 行星详情仍暂停.
  stellarRevolutionSpeed: 15,
  // 黑洞同属恒星, 在原 0.75 倍线性尺寸上体积翻倍; 盘面、阴影与轨道障碍半径同步缩放.
  blackHoleScale: 0.75 * Math.cbrt(2),
  // 卫星公转虚拟时间倍率; 选中行星时统一暂停, 恒星使用独立的 stellarRevolutionSpeed.
  orbitSpeed: {
    // 总览时加快整体公转.
    overview: 15,
    // 聚焦恒星后减速, 便于选择行星.
    star: 6,
  },
  // 普通恒星默认一分钟一圈, 单位弧度/秒; 脉冲星默认值由 pulsar/config.ts 定义, 均可按节点覆盖.
  starRotationRadiansPerSecond: (Math.PI * 2) / 60,
  camera: {
    // 垂直视场角, 单位度, 必须处于 0..180 之间.
    fieldOfView: 45,
    // 近裁剪距离, 正数且小于 far.
    near: 0.1,
    // 默认远裁剪距离; 实际值随星图包围范围扩大.
    far: 1600,
    // 手动缩放允许的最近观察距离, 防止进入星体内部.
    minDistance: 8,
    // 默认最远观察距离; 大星图的总览、手动缩放与恒星聚焦共享扩大后的边界.
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
    // 首次加载、重建和返回总览共用的精确世界坐标, 不被自动构图覆盖.
    initialPosition: [-178, 176, 1083],
    // 总览相机观察中心的世界坐标 [x, y, z].
    overviewTarget: [0, 0, 16],
  },
} as const;
