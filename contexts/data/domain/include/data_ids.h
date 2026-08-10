#pragma once

#include <cstdint>

#include "data_config.h"
#include "data_model.h"

// dataId 清单：cell 500 个（id 1-500，1S 周期，needcache=true，cache 8 字节）。
// raw 布局：[u16 dataType][u32 cellId][u32 cpId][500 × 8字节切片]；extractor i 切 head + (i-1)*8。
// 换真实清单只改本列表。
// X(dataId, 类型名, 周期tick数, needcache)

#define DTS_CELL_DATA_IDS(X) \
    X(1, CellData001, 10, true) X(2, CellData002, 10, true) X(3, CellData003, 10, true) X(4, CellData004, 10, true) X(5, CellData005, 10, true) \
    X(6, CellData006, 10, true) X(7, CellData007, 10, true) X(8, CellData008, 10, true) X(9, CellData009, 10, true) X(10, CellData010, 10, true) \
    X(11, CellData011, 10, true) X(12, CellData012, 10, true) X(13, CellData013, 10, true) X(14, CellData014, 10, true) X(15, CellData015, 10, true) \
    X(16, CellData016, 10, true) X(17, CellData017, 10, true) X(18, CellData018, 10, true) X(19, CellData019, 10, true) X(20, CellData020, 10, true) \
    X(21, CellData021, 10, true) X(22, CellData022, 10, true) X(23, CellData023, 10, true) X(24, CellData024, 10, true) X(25, CellData025, 10, true) \
    X(26, CellData026, 10, true) X(27, CellData027, 10, true) X(28, CellData028, 10, true) X(29, CellData029, 10, true) X(30, CellData030, 10, true) \
    X(31, CellData031, 10, true) X(32, CellData032, 10, true) X(33, CellData033, 10, true) X(34, CellData034, 10, true) X(35, CellData035, 10, true) \
    X(36, CellData036, 10, true) X(37, CellData037, 10, true) X(38, CellData038, 10, true) X(39, CellData039, 10, true) X(40, CellData040, 10, true) \
    X(41, CellData041, 10, true) X(42, CellData042, 10, true) X(43, CellData043, 10, true) X(44, CellData044, 10, true) X(45, CellData045, 10, true) \
    X(46, CellData046, 10, true) X(47, CellData047, 10, true) X(48, CellData048, 10, true) X(49, CellData049, 10, true) X(50, CellData050, 10, true) \
    X(51, CellData051, 10, true) X(52, CellData052, 10, true) X(53, CellData053, 10, true) X(54, CellData054, 10, true) X(55, CellData055, 10, true) \
    X(56, CellData056, 10, true) X(57, CellData057, 10, true) X(58, CellData058, 10, true) X(59, CellData059, 10, true) X(60, CellData060, 10, true) \
    X(61, CellData061, 10, true) X(62, CellData062, 10, true) X(63, CellData063, 10, true) X(64, CellData064, 10, true) X(65, CellData065, 10, true) \
    X(66, CellData066, 10, true) X(67, CellData067, 10, true) X(68, CellData068, 10, true) X(69, CellData069, 10, true) X(70, CellData070, 10, true) \
    X(71, CellData071, 10, true) X(72, CellData072, 10, true) X(73, CellData073, 10, true) X(74, CellData074, 10, true) X(75, CellData075, 10, true) \
    X(76, CellData076, 10, true) X(77, CellData077, 10, true) X(78, CellData078, 10, true) X(79, CellData079, 10, true) X(80, CellData080, 10, true) \
    X(81, CellData081, 10, true) X(82, CellData082, 10, true) X(83, CellData083, 10, true) X(84, CellData084, 10, true) X(85, CellData085, 10, true) \
    X(86, CellData086, 10, true) X(87, CellData087, 10, true) X(88, CellData088, 10, true) X(89, CellData089, 10, true) X(90, CellData090, 10, true) \
    X(91, CellData091, 10, true) X(92, CellData092, 10, true) X(93, CellData093, 10, true) X(94, CellData094, 10, true) X(95, CellData095, 10, true) \
    X(96, CellData096, 10, true) X(97, CellData097, 10, true) X(98, CellData098, 10, true) X(99, CellData099, 10, true) X(100, CellData100, 10, true) \
    X(101, CellData101, 10, true) X(102, CellData102, 10, true) X(103, CellData103, 10, true) X(104, CellData104, 10, true) X(105, CellData105, 10, true) \
    X(106, CellData106, 10, true) X(107, CellData107, 10, true) X(108, CellData108, 10, true) X(109, CellData109, 10, true) X(110, CellData110, 10, true) \
    X(111, CellData111, 10, true) X(112, CellData112, 10, true) X(113, CellData113, 10, true) X(114, CellData114, 10, true) X(115, CellData115, 10, true) \
    X(116, CellData116, 10, true) X(117, CellData117, 10, true) X(118, CellData118, 10, true) X(119, CellData119, 10, true) X(120, CellData120, 10, true) \
    X(121, CellData121, 10, true) X(122, CellData122, 10, true) X(123, CellData123, 10, true) X(124, CellData124, 10, true) X(125, CellData125, 10, true) \
    X(126, CellData126, 10, true) X(127, CellData127, 10, true) X(128, CellData128, 10, true) X(129, CellData129, 10, true) X(130, CellData130, 10, true) \
    X(131, CellData131, 10, true) X(132, CellData132, 10, true) X(133, CellData133, 10, true) X(134, CellData134, 10, true) X(135, CellData135, 10, true) \
    X(136, CellData136, 10, true) X(137, CellData137, 10, true) X(138, CellData138, 10, true) X(139, CellData139, 10, true) X(140, CellData140, 10, true) \
    X(141, CellData141, 10, true) X(142, CellData142, 10, true) X(143, CellData143, 10, true) X(144, CellData144, 10, true) X(145, CellData145, 10, true) \
    X(146, CellData146, 10, true) X(147, CellData147, 10, true) X(148, CellData148, 10, true) X(149, CellData149, 10, true) X(150, CellData150, 10, true) \
    X(151, CellData151, 10, true) X(152, CellData152, 10, true) X(153, CellData153, 10, true) X(154, CellData154, 10, true) X(155, CellData155, 10, true) \
    X(156, CellData156, 10, true) X(157, CellData157, 10, true) X(158, CellData158, 10, true) X(159, CellData159, 10, true) X(160, CellData160, 10, true) \
    X(161, CellData161, 10, true) X(162, CellData162, 10, true) X(163, CellData163, 10, true) X(164, CellData164, 10, true) X(165, CellData165, 10, true) \
    X(166, CellData166, 10, true) X(167, CellData167, 10, true) X(168, CellData168, 10, true) X(169, CellData169, 10, true) X(170, CellData170, 10, true) \
    X(171, CellData171, 10, true) X(172, CellData172, 10, true) X(173, CellData173, 10, true) X(174, CellData174, 10, true) X(175, CellData175, 10, true) \
    X(176, CellData176, 10, true) X(177, CellData177, 10, true) X(178, CellData178, 10, true) X(179, CellData179, 10, true) X(180, CellData180, 10, true) \
    X(181, CellData181, 10, true) X(182, CellData182, 10, true) X(183, CellData183, 10, true) X(184, CellData184, 10, true) X(185, CellData185, 10, true) \
    X(186, CellData186, 10, true) X(187, CellData187, 10, true) X(188, CellData188, 10, true) X(189, CellData189, 10, true) X(190, CellData190, 10, true) \
    X(191, CellData191, 10, true) X(192, CellData192, 10, true) X(193, CellData193, 10, true) X(194, CellData194, 10, true) X(195, CellData195, 10, true) \
    X(196, CellData196, 10, true) X(197, CellData197, 10, true) X(198, CellData198, 10, true) X(199, CellData199, 10, true) X(200, CellData200, 10, true) \
    X(201, CellData201, 10, true) X(202, CellData202, 10, true) X(203, CellData203, 10, true) X(204, CellData204, 10, true) X(205, CellData205, 10, true) \
    X(206, CellData206, 10, true) X(207, CellData207, 10, true) X(208, CellData208, 10, true) X(209, CellData209, 10, true) X(210, CellData210, 10, true) \
    X(211, CellData211, 10, true) X(212, CellData212, 10, true) X(213, CellData213, 10, true) X(214, CellData214, 10, true) X(215, CellData215, 10, true) \
    X(216, CellData216, 10, true) X(217, CellData217, 10, true) X(218, CellData218, 10, true) X(219, CellData219, 10, true) X(220, CellData220, 10, true) \
    X(221, CellData221, 10, true) X(222, CellData222, 10, true) X(223, CellData223, 10, true) X(224, CellData224, 10, true) X(225, CellData225, 10, true) \
    X(226, CellData226, 10, true) X(227, CellData227, 10, true) X(228, CellData228, 10, true) X(229, CellData229, 10, true) X(230, CellData230, 10, true) \
    X(231, CellData231, 10, true) X(232, CellData232, 10, true) X(233, CellData233, 10, true) X(234, CellData234, 10, true) X(235, CellData235, 10, true) \
    X(236, CellData236, 10, true) X(237, CellData237, 10, true) X(238, CellData238, 10, true) X(239, CellData239, 10, true) X(240, CellData240, 10, true) \
    X(241, CellData241, 10, true) X(242, CellData242, 10, true) X(243, CellData243, 10, true) X(244, CellData244, 10, true) X(245, CellData245, 10, true) \
    X(246, CellData246, 10, true) X(247, CellData247, 10, true) X(248, CellData248, 10, true) X(249, CellData249, 10, true) X(250, CellData250, 10, true) \
    X(251, CellData251, 10, true) X(252, CellData252, 10, true) X(253, CellData253, 10, true) X(254, CellData254, 10, true) X(255, CellData255, 10, true) \
    X(256, CellData256, 10, true) X(257, CellData257, 10, true) X(258, CellData258, 10, true) X(259, CellData259, 10, true) X(260, CellData260, 10, true) \
    X(261, CellData261, 10, true) X(262, CellData262, 10, true) X(263, CellData263, 10, true) X(264, CellData264, 10, true) X(265, CellData265, 10, true) \
    X(266, CellData266, 10, true) X(267, CellData267, 10, true) X(268, CellData268, 10, true) X(269, CellData269, 10, true) X(270, CellData270, 10, true) \
    X(271, CellData271, 10, true) X(272, CellData272, 10, true) X(273, CellData273, 10, true) X(274, CellData274, 10, true) X(275, CellData275, 10, true) \
    X(276, CellData276, 10, true) X(277, CellData277, 10, true) X(278, CellData278, 10, true) X(279, CellData279, 10, true) X(280, CellData280, 10, true) \
    X(281, CellData281, 10, true) X(282, CellData282, 10, true) X(283, CellData283, 10, true) X(284, CellData284, 10, true) X(285, CellData285, 10, true) \
    X(286, CellData286, 10, true) X(287, CellData287, 10, true) X(288, CellData288, 10, true) X(289, CellData289, 10, true) X(290, CellData290, 10, true) \
    X(291, CellData291, 10, true) X(292, CellData292, 10, true) X(293, CellData293, 10, true) X(294, CellData294, 10, true) X(295, CellData295, 10, true) \
    X(296, CellData296, 10, true) X(297, CellData297, 10, true) X(298, CellData298, 10, true) X(299, CellData299, 10, true) X(300, CellData300, 10, true) \
    X(301, CellData301, 10, true) X(302, CellData302, 10, true) X(303, CellData303, 10, true) X(304, CellData304, 10, true) X(305, CellData305, 10, true) \
    X(306, CellData306, 10, true) X(307, CellData307, 10, true) X(308, CellData308, 10, true) X(309, CellData309, 10, true) X(310, CellData310, 10, true) \
    X(311, CellData311, 10, true) X(312, CellData312, 10, true) X(313, CellData313, 10, true) X(314, CellData314, 10, true) X(315, CellData315, 10, true) \
    X(316, CellData316, 10, true) X(317, CellData317, 10, true) X(318, CellData318, 10, true) X(319, CellData319, 10, true) X(320, CellData320, 10, true) \
    X(321, CellData321, 10, true) X(322, CellData322, 10, true) X(323, CellData323, 10, true) X(324, CellData324, 10, true) X(325, CellData325, 10, true) \
    X(326, CellData326, 10, true) X(327, CellData327, 10, true) X(328, CellData328, 10, true) X(329, CellData329, 10, true) X(330, CellData330, 10, true) \
    X(331, CellData331, 10, true) X(332, CellData332, 10, true) X(333, CellData333, 10, true) X(334, CellData334, 10, true) X(335, CellData335, 10, true) \
    X(336, CellData336, 10, true) X(337, CellData337, 10, true) X(338, CellData338, 10, true) X(339, CellData339, 10, true) X(340, CellData340, 10, true) \
    X(341, CellData341, 10, true) X(342, CellData342, 10, true) X(343, CellData343, 10, true) X(344, CellData344, 10, true) X(345, CellData345, 10, true) \
    X(346, CellData346, 10, true) X(347, CellData347, 10, true) X(348, CellData348, 10, true) X(349, CellData349, 10, true) X(350, CellData350, 10, true) \
    X(351, CellData351, 10, true) X(352, CellData352, 10, true) X(353, CellData353, 10, true) X(354, CellData354, 10, true) X(355, CellData355, 10, true) \
    X(356, CellData356, 10, true) X(357, CellData357, 10, true) X(358, CellData358, 10, true) X(359, CellData359, 10, true) X(360, CellData360, 10, true) \
    X(361, CellData361, 10, true) X(362, CellData362, 10, true) X(363, CellData363, 10, true) X(364, CellData364, 10, true) X(365, CellData365, 10, true) \
    X(366, CellData366, 10, true) X(367, CellData367, 10, true) X(368, CellData368, 10, true) X(369, CellData369, 10, true) X(370, CellData370, 10, true) \
    X(371, CellData371, 10, true) X(372, CellData372, 10, true) X(373, CellData373, 10, true) X(374, CellData374, 10, true) X(375, CellData375, 10, true) \
    X(376, CellData376, 10, true) X(377, CellData377, 10, true) X(378, CellData378, 10, true) X(379, CellData379, 10, true) X(380, CellData380, 10, true) \
    X(381, CellData381, 10, true) X(382, CellData382, 10, true) X(383, CellData383, 10, true) X(384, CellData384, 10, true) X(385, CellData385, 10, true) \
    X(386, CellData386, 10, true) X(387, CellData387, 10, true) X(388, CellData388, 10, true) X(389, CellData389, 10, true) X(390, CellData390, 10, true) \
    X(391, CellData391, 10, true) X(392, CellData392, 10, true) X(393, CellData393, 10, true) X(394, CellData394, 10, true) X(395, CellData395, 10, true) \
    X(396, CellData396, 10, true) X(397, CellData397, 10, true) X(398, CellData398, 10, true) X(399, CellData399, 10, true) X(400, CellData400, 10, true) \
    X(401, CellData401, 10, true) X(402, CellData402, 10, true) X(403, CellData403, 10, true) X(404, CellData404, 10, true) X(405, CellData405, 10, true) \
    X(406, CellData406, 10, true) X(407, CellData407, 10, true) X(408, CellData408, 10, true) X(409, CellData409, 10, true) X(410, CellData410, 10, true) \
    X(411, CellData411, 10, true) X(412, CellData412, 10, true) X(413, CellData413, 10, true) X(414, CellData414, 10, true) X(415, CellData415, 10, true) \
    X(416, CellData416, 10, true) X(417, CellData417, 10, true) X(418, CellData418, 10, true) X(419, CellData419, 10, true) X(420, CellData420, 10, true) \
    X(421, CellData421, 10, true) X(422, CellData422, 10, true) X(423, CellData423, 10, true) X(424, CellData424, 10, true) X(425, CellData425, 10, true) \
    X(426, CellData426, 10, true) X(427, CellData427, 10, true) X(428, CellData428, 10, true) X(429, CellData429, 10, true) X(430, CellData430, 10, true) \
    X(431, CellData431, 10, true) X(432, CellData432, 10, true) X(433, CellData433, 10, true) X(434, CellData434, 10, true) X(435, CellData435, 10, true) \
    X(436, CellData436, 10, true) X(437, CellData437, 10, true) X(438, CellData438, 10, true) X(439, CellData439, 10, true) X(440, CellData440, 10, true) \
    X(441, CellData441, 10, true) X(442, CellData442, 10, true) X(443, CellData443, 10, true) X(444, CellData444, 10, true) X(445, CellData445, 10, true) \
    X(446, CellData446, 10, true) X(447, CellData447, 10, true) X(448, CellData448, 10, true) X(449, CellData449, 10, true) X(450, CellData450, 10, true) \
    X(451, CellData451, 10, true) X(452, CellData452, 10, true) X(453, CellData453, 10, true) X(454, CellData454, 10, true) X(455, CellData455, 10, true) \
    X(456, CellData456, 10, true) X(457, CellData457, 10, true) X(458, CellData458, 10, true) X(459, CellData459, 10, true) X(460, CellData460, 10, true) \
    X(461, CellData461, 10, true) X(462, CellData462, 10, true) X(463, CellData463, 10, true) X(464, CellData464, 10, true) X(465, CellData465, 10, true) \
    X(466, CellData466, 10, true) X(467, CellData467, 10, true) X(468, CellData468, 10, true) X(469, CellData469, 10, true) X(470, CellData470, 10, true) \
    X(471, CellData471, 10, true) X(472, CellData472, 10, true) X(473, CellData473, 10, true) X(474, CellData474, 10, true) X(475, CellData475, 10, true) \
    X(476, CellData476, 10, true) X(477, CellData477, 10, true) X(478, CellData478, 10, true) X(479, CellData479, 10, true) X(480, CellData480, 10, true) \
    X(481, CellData481, 10, true) X(482, CellData482, 10, true) X(483, CellData483, 10, true) X(484, CellData484, 10, true) X(485, CellData485, 10, true) \
    X(486, CellData486, 10, true) X(487, CellData487, 10, true) X(488, CellData488, 10, true) X(489, CellData489, 10, true) X(490, CellData490, 10, true) \
    X(491, CellData491, 10, true) X(492, CellData492, 10, true) X(493, CellData493, 10, true) X(494, CellData494, 10, true) X(495, CellData495, 10, true) \
    X(496, CellData496, 10, true) X(497, CellData497, 10, true) X(498, CellData498, 10, true) X(499, CellData499, 10, true) X(500, CellData500, 10, true)

namespace dts::data {

// 生成 dataId 结构（模拟：2 个 int，8 字节）
#define DEFINE_CACHE(id, Name, period, need) \
    struct Name { int f0; int f1; };
DTS_CELL_DATA_IDS(DEFINE_CACHE)
#undef DEFINE_CACHE

// 注册规格表：dataId → (dataType, cacheSize, 周期, needcache)
struct DataIdSpec {
    uint16_t dataId;
    uint16_t dataType;      // DataType::CELL
    uint32_t cacheSize;     // sizeof(dataId 结构)
    uint32_t periodTicks;
    bool needCache;
};

#define SPEC_CELL(id, Name, period, need) \
    { id, static_cast<uint16_t>(DataType::CELL), sizeof(Name), period, need },
inline constexpr DataIdSpec kCellSpecs[] = { DTS_CELL_DATA_IDS(SPEC_CELL) };
#undef SPEC_CELL

// dataId → spec 查找
inline const DataIdSpec* SpecOf(uint16_t dataId) {
    for (const auto& s : kCellSpecs) {
        if (s.dataId == dataId) return &s;
    }
    return nullptr;
}

inline constexpr uint16_t kCellDataIdBase = 1;
inline constexpr uint16_t kCellDataIdCount = 500;

}  // namespace dts::data
