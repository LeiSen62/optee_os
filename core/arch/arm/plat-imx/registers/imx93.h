/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright 2022-2023, 2025 NXP
 */
#ifndef __IMX93_H__
#define __IMX93_H__

#define GICD_BASE 0x48000000
#define GICR_BASE 0x48040000

#define UART1_BASE 0x44380000
/*
 * For Normal MU - Use MU_BASE as 0x47520000
 * For Trust MU - Use MU_BASE as 0x47530000
 */
#define MU_BASE 0x47530000
#define MU_SIZE	   0x10000

#define MU_TRUST_BASE 0x47530000

#endif /* __IMX93_H__ */
