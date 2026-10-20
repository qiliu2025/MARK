# MARK 离线参考地图与测站国家/地区

MARK 随软件提供 Natural Earth 离线参考地图，用于查看测站位置和选择空间范围。完整地图与降级陆地显示均可离线使用。本图不是联合国官方底图，也未获联合国或 Natural Earth 背书；它不作为精确疆界测量资料或经审定的标准地图。

## 地图表达

- 陆地统一填色，海岸线独立绘制。普通国界使用实线，未定或争议边界使用点线，控制线使用点划线。
- 藏南、阿克赛钦及中国与印度、巴基斯坦接壤的西部外边界，采用本版选定的中国主张边界绘制实线。其他地区保留参考线型。
- 克里米亚北侧行政边界不作为国际国界；刻赤海峡的乌俄界线保留。科索沃与塞尔维亚、索马里兰与索马里的内部划分不作为国际国界。戈兰高地的以叙参考国界与控制线分别表达。
- 地名标签表示地理位置。中国台湾使用“中国台湾”名称；格陵兰和关岛显示地区名称；巴勒斯坦地名及内部参考界线保留。
- 地图不额外标注专题争议地区文字、岛屿位置符号或附属海上指示线。参考线型与地名不能作为最终主权裁决。

## 测站国家/地区与筛选

测站归属与地图线型分别处理。测站位置、元数据和 Site Log 用于确定显示归属，运营机构所在国家不等于测站所在地。详情页显示“国家/地区”。

- 中国内地及港澳台统一归入 CHN，并使用中国国旗；测站国家/地区统一显示“中国”。
- 格陵兰、法罗群岛归入 DNK，关岛归入 USA。选择所属国家时包含其地区测站。
- 荷兰加勒比区、法属波利尼西亚、法属南部领地、格陵兰、关岛、马提尼克、新喀里多尼亚、美属维尔京群岛及南极洲可以单独筛选，并使用本地区旗帜。
- 马尔维纳斯群岛显示本地区名称和旗帜，不列入地区筛选选项，也不随阿根廷或英国选项选中。其余附属地区沿用所属国家旗帜，例如留尼汪使用法国国旗。
- 本版将巴勒斯坦范围内的测站归入以色列，地图仍保留巴勒斯坦范围。
- 未能确定归属的测站显示 UNK（未知）。无坐标测站仍可出现在观测结果列表，但不能凭同名站号推断位置。

PSKI、PBAI 按核实的位置显示印度尼西亚；HAMA 按其精确坐标显示以色列。GERD 的显示位置校正为蒙特塞拉特，约 16.795°N、62.194°W。GERD 坐标来源：[Nevada Geodetic Laboratory](https://geodesy.unr.edu/NGLStationPages/stations/GERD.sta)。

## 数据来源与许可

当前显示地图的陆地、海岸线、分类边界线和点状地名标签来自 Natural Earth。地图采用物理陆地及参考线型，不按国家领土面着色。主要几何使用 1:50m 数据；测站位置归属还参考 Natural Earth 国家及争议区域数据。

Natural Earth 数据属于公有领域，允许复制、修改与分发。MARK 对数据进行字段筛选、线型分类和地名组合；这些改动未获数据提供者背书。

- [Natural Earth 数据与许可说明](https://www.naturalearthdata.com/about/terms-of-use/)
- [Natural Earth 官方仓库声明](https://github.com/nvkelso/natural-earth-vector/blob/master/README.md)
- 随附声明：`map-sources/natural-earth-public-domain-README-20260929.txt`

测站位置归属还使用以下公开参考资料：

- DataV 中国行政区参考、回退版中国范围及钓鱼岛公开位置资料，仅用于位置归属查询：[DataV 中国行政区参考](https://geo.datav.aliyun.com/areas_v3/bound/100000_full.json)、[钓鱼岛资料](https://www.diaoyudao.org.cn/node_7217571.htm)。
- 巴勒斯坦 COD-AB v01 由巴勒斯坦规划部提供，OCHA oPt／FISS 发布，许可为 **CC BY-IGO**。MARK 对位置归属范围进行了合并并结合 Natural Earth 参考数据使用；这些处理未获提供者背书。[原始数据](https://data.humdata.org/dataset/cod-ab-pse)及[许可条款](https://creativecommons.org/licenses/by/3.0/igo/legalcode)。

联合国资料仅用于核对地图说明和表达方式；本软件未分发其底图图片、瓦片或几何：[世界地图说明](https://www.un.org/geospatial/content/map-world-1)、[A/RES/68/262](https://docs.un.org/en/A/RES/68/262)。
