# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2018-2020 Oplus. All rights reserved.
#Kun.Hu@TECH.BSP.Stability.PHOENIX_PROJECT 2019/06/15, Add for PHOENIX_PROJECT
ifdef CONFIG_OPLUS_SYSTEM_KERNEL_QCOM
obj-$(CONFIG_OPLUS_FEATURE_PHOENIX) += op_bootprof.o
obj-$(CONFIG_OPLUS_FEATURE_PHOENIX) += phoenix_base.o
obj-$(CONFIG_OPLUS_FEATURE_PHOENIX) += phoenix_dump.o
obj-$(CONFIG_OPLUS_FEATURE_PHOENIX) += phoenix_watchdog.o
else
ifeq ($(CONFIG_OPLUS_FEATURE_PHOENIX), y)
obj-$(CONFIG_OPLUS_FEATURE_PHOENIX) += op_bootprof.o
obj-$(CONFIG_OPLUS_FEATURE_PHOENIX) += phoenix_base.o
obj-$(CONFIG_OPLUS_FEATURE_PHOENIX) += phoenix_dump.o
obj-$(CONFIG_OPLUS_FEATURE_PHOENIX) += phoenix_watchdog.o
endif
ifeq ($(CONFIG_OPLUS_FEATURE_PHOENIX), m)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include
obj-$(CONFIG_OPLUS_FEATURE_PHOENIX) += oplus_phoenix.o
oplus_phoenix-y := gki/op_bootprof.o gki/phoenix_base.o gki/phoenix_dump.o gki/phoenix_watchdog.o
endif
endif

obj-$(CONFIG_OPLUS_FEATURE_KMSG_WB) += oplus_kmsg_wb.o

#ifdef OPLUS_BUG_STABILITY
#leiwuyue@BSP.Kernel.Stability,2022/07/11 for bug3861222
ifeq ($(MTK_PLATFORM),mt6779)
###ODEX time out for first time,extend time
CFLAGS_KERNEL += -DPHOENIX_LONG_TIME
CFLAGS_MODULE += -DPHOENIX_LONG_TIME
endif
#endif OPLUS_BUG_STABILITY
