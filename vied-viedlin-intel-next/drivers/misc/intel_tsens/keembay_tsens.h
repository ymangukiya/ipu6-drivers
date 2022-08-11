/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *
 * Intel Keem Bay thermal Driver
 *
 * Copyright (C) 2020 Intel Corporation
 *
 */

#ifndef _LINUX_KEEMBAY_TSENS_H
#define _LINUX_KEEMBAY_TSENS_H

#include <linux/thermal.h>

/* Register values for keembay temperature (PVT Sensor) */
#define AON_TSENS_TRIM0_CFG		0x0030
#define AON_TSENS_TRIM1_CFG		0x0034
#define AON_TSENS_CFG			0x0038
#define AON_TSENS_INT0			0x203c
#define AON_TSENS_INT1			0x2040
#define AON_TSENS_IRQ_CLEAR		0x0044
#define AON_TSENS_DATA0			0x0048
#define MSS_T_SAMPLE_VALID		0x80000000
#define MSS_T_SAMPLE			0x3ff
#define CSS_T_SAMPLE_VALID		0x8000
#define CSS_T_SAMPLE			0x3ff
#define NCE1_T_SAMPLE_VALID		0x80000000
#define NCE1_T_SAMPLE			0x3ff
#define NCE0_T_SAMPLE_VALID		0x8000
#define NCE0_T_SAMPLE			0x3ff
#define AON_TSENS_DATA1			0x004c
#define AON_INTERFACE			0x20260000
/* Bit shift for registers*/
#define MSS_BIT_SHIFT			16
#define CSS_BIT_SHIFT			0
#define NCE0_BIT_SHIFT			0
#define NCE1_BIT_SHIFT			16
/* mask values for config register */
#define CFG_MASK_AUTO			0x80ff //(auto configuration)
#define CFG_IRQ_MASK			0x8fff
#define CFG_MASK_MANUAL		0x000f // TSENS_EN (manual config)

/**
 * KEEMBAY_SENSOR_MSS - Media subsystem junction temperature.
 * KEEMBAY_SENSOR_CSS - Compute subsystem junction temperature.
 * KEEMBAY_SENSOR_NCE - Neural computing engine junction temperature.
 *			For NCE two sensors are available in kemmaby paltform,
 *			maximum temperature of these two sensors will be
 *			returned as NCE temperature.
 * KEEMBAY_SENSOR_SOC - Soc temperature.
 *			Maximum of MSS, CSS and NCE would be returned as
 *			SOC temperature.
 */
enum keembay_thermal_sensor_en {
	KEEMBAY_SENSOR_MSS,
	KEEMBAY_SENSOR_CSS,
	KEEMBAY_SENSOR_NCE,
	KEEMBAY_SENSOR_SOC,
	KEEMBAY_SENSOR_MAX
};

#define KEEMBAY_SENSOR_BASE_TEMP 27

static int const raw_kmb[] = {
39956,  -39637, -39319, -39001, -38684,

38367,  -38050, -37734, -37418, -37103,

36787,  -36472, -36158, -35844, -35530,

35216,  -34903, -34590, -34278, -33966,

33654,  -33343, -33032, -32721, -32411,

32101,  -31791, -31482, -31173, -30864,

30556,  -30248, -29940, -29633, -29326,

29020,  -28713, -28407, -28102, -27797,

27492,  -27187, -26883, -26579, -26276,

25973,  -25670, -25367, -25065, -24763,

24462,  -24160, -23860, -23559, -23259,

22959,  -22660, -22360, -22062, -21763,

21465,  -21167, -20869, -20572, -20275,

19979,  -19683, -19387, -19091, -18796,

18501,  -18206, -17912, -17618, -17325,

-17031, -16738,  -16446, -16153, -15861,

-15570, -15278,  -14987, -14697, -14406,

-14116, -13826,  -13537, -13248, -12959,

-12670, -12382,  -12094, -11807, -11520,

-11233, -10946,  -10660, -10374, -10088,

-9803, -9518, -9233, -8949, -8665,

-8381, -8097, -7814, -7531, -7249,

-6967, -6685, -6403, -6122, -5841,

-5560, -5279, -4999, -4720, -4440,

-4161, -3882, -3603, -3325, -3047,

-2770, -2492, -2215, -1938, -1662,

-1386, -1110, -834, -559, -284,

-9, 265, 539, 813, 1086,

1360, 1633, 1905, 2177, 2449,

2721, 2993, 3264, 3535, 3805,

4075, 4345, 4615, 4884, 5153,

5422, 5691, 5959, 6227, 6495,

6762, 7029, 7296, 7562, 7829,

8095, 8360, 8626, 8891, 9155,

9420, 9684, 9948, 10212, 10475,

10738, 11001, 11264, 11526, 11788,

12049, 12311, 12572, 12833, 13093,

13354, 13614, 13874, 14133, 14392,

14651, 14910, 15168, 15426, 15684,

15942, 16199, 16456, 16713, 16969,

17225, 17481, 17737, 17992, 18247,

18502, 18757, 19011, 19265, 19519,

19772, 20025, 20278, 20531, 20784,

21036, 21288, 21539, 21791, 22042,

22292, 22543, 22793, 23043, 23293,

23543, 23792, 24041, 24290, 24538,

24786, 25034, 25282, 25529, 25776,

26023, 26270, 26516, 26763, 27008,

27254, 27499, 27745, 27989, 28234,

28478, 28722, 28966, 29210, 29453,

29696, 29939, 30182, 30424, 30666,

30908, 31149, 31391, 31632, 31873,

32113, 32353, 32593, 32833, 33073,

33312, 33551, 33790, 34029, 34267,

34505, 34743, 34980, 35218, 35455,

35692, 35928, 36165, 36401, 36637,

36872, 37108, 37343, 37578, 37813,

38047, 38281, 38515, 38749, 38982,

39216, 39448, 39681, 39914, 40146,

40378, 40610, 40841, 41073, 41304,

41535, 41765, 41996, 42226, 42456,

42686, 42915, 43144, 43373, 43602,

43830, 44059, 44287, 44515, 44742,

44970, 45197, 45424, 45650, 45877,

46103, 46329, 46555, 46780, 47006,

47231, 47456, 47680, 47905, 48129,

48353, 48576, 48800,  49023, 49246,

49469, 49692, 49914,  50136, 50358,

50580, 50801, 51023,  51244, 51464,

51685, 51905, 52126,  52346, 52565,

52785, 53004, 53223,  53442, 53661,

53879, 54097, 54315,  54533, 54750,

54968, 55185, 55402,  55618, 55835,

56051, 56267, 56483,  56699, 56914,

57129, 57344, 57559,  57773, 57988,

58202, 58416, 58630,  58843, 59056,

59269, 59482, 59695,  59907, 60120,

60332, 60543, 60755,  60966, 61178,

61389, 61599, 61810,  62020, 62231,

62440, 62650, 62860,  63069, 63278,

63487, 63696, 63904,  64113, 64321,

64529, 64737, 64944,  65151, 65358,

65565, 65772, 65979,  66185, 66391,

66597, 66803, 67008, 67213, 67419,

67624, 67828, 68033, 68237, 68441,

68645, 68849, 69052, 69256, 69459,

69662, 69865, 70067, 70270, 70472,

70674, 70876, 71077, 71279, 71480,

71681, 71882, 72082, 72283, 72483,

72683, 72883, 73083, 73282, 73481,

73680, 73879, 74078, 74277, 74475,

74673, 74871, 75069, 75266, 75464,

75661, 75858, 76055, 76252, 76448,

76644, 76841, 77037, 77232, 77428,

77623, 77818, 78013, 78208, 78403,

78597, 78792, 78986, 79180, 79373,

79567, 79760, 79953, 80146, 80339,

80532, 80724, 80917, 81109, 81301,

81492, 81684, 81875, 82066, 82258,

82448, 82639, 82830, 83020, 83210,

83400, 83590, 83779, 83969, 84158,

84347, 84536, 84725, 84913, 85102,

85290, 85478, 85666, 85854, 86041,

86228, 86416, 86603, 86789, 86976,

87163, 87349, 87535, 87721, 87907,

88092, 88278, 88463, 88648, 88833,

89018, 89203, 89387, 89571, 89755,

89939, 90123, 90307, 90490, 90674,

90857, 91040, 91222, 91405, 91587,

91770, 91952, 92134, 92315, 92497,

92679, 92860, 93041, 93222, 93403,

93583, 93764, 93944, 94124, 94304,

94484, 94664, 94843, 95023, 95202,

95381, 95560, 95738, 95917, 96095,

96273, 96451, 96629, 96807, 96985,

97162, 97339, 97516, 97693, 97870,

98047, 98223, 98399, 98576, 98752,

98927, 99103, 99279, 99454, 99629,

99804, 99979, 100154, 100328, 100503,

100677, 100851, 101025, 101199, 101373,

101546, 101720, 101893, 102066, 102239,

102411, 102584, 102756, 102929, 103101,

103273, 103445, 103616, 103788, 103959,

104130, 104302, 104472, 104643, 104814,

104984, 105155, 105325, 105495, 105665,

105835, 106004, 106174, 106343, 106512,

106681, 106850, 107019, 107187, 107355,

107524, 107692, 107860, 108028, 108195,

108363, 108530, 108697, 108865, 109031,

109198, 109365, 109531, 109698, 109864,

110030, 110196, 110362, 110528, 110693,

110858, 111024, 111189, 111354, 111518,

111683, 111848, 112012, 112176, 112340,

112504, 112668, 112832, 112995, 113159,

113322, 113485, 113648, 113811, 113973,

114136, 114298, 114461, 114623, 114785,

114947, 115108, 115270, 115431, 115593,

115754, 115915, 116076, 116236, 116397,

116558, 116718, 116878, 117038, 117198,

117358, 117518, 117677, 117836, 117996,

118155, 118314, 118473, 118631, 118790,

118948, 119107, 119265, 119423, 119581,

119739, 119896, 120054, 120211, 120368,

120525, 120682, 120839, 120996, 121153,

121309, 121465, 121622, 121778, 121934,

122089, 122245, 122400, 122556, 122711,

122866, 123021, 123176, 123331, 123486,

123640, 123794, 123949, 124103, 124257,

124411, 124564, 124718, 124871, 125025,
};

static int raw_kmb_size = sizeof(raw_kmb) / sizeof(int);

#endif /* _LINUX_KEEMBAY_TSENS_H */
