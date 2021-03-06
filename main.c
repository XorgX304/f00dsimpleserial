/* simpleserial for f00d
 *
 * Copyright (C) 2018 molecule
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */
#include "config.h"
#include "types.h"

#include "uart.h"
#include "pervasive.h"
#include "gpio.h"
#include "simpleserial.h"
#include "aes.h"

static volatile u32_t * const BIGMAC = (void *)0xE0050000;
static volatile void * const BIGMAC_KEY = (void *)0xE0050200;
static u32_t g_keyslot;
static u32_t g_len;
static u32_t g_key[8];
static u32_t g_dst_keyslot;

void *memcpy(void *dst, const void *src, size_t n) {
  u32_t *dst32 = dst;
  const u32_t *src32 = src;
  size_t i;
  for (i = 0; i < (n/4); i++) {
    dst32[i] = src32[i];
  }
  u8_t *dst8 = dst;
  const u8_t *src8 = src;
  size_t j;
  for (j = i*4; j < n; j++) {
    dst8[j] = src8[j];
  }
  return dst;
}

static u8_t get_key128(u8_t* k)
{
  memcpy(g_key, k, 16);
  AES128_ECB_indp_setkey(k);
  g_keyslot = 0;
  g_len = 16;
  return 0x00;
}

static u8_t get_key256(u8_t* k)
{
  memcpy(g_key, k, 32);
  g_keyslot = 0;
  g_len = 32;
  return 0x00;
}

static void trigger_high(void)
{
  gpio_port_set(0, GPIO_PORT_PS_LED);
}

static void trigger_low(void)
{
  gpio_port_clear(0, GPIO_PORT_PS_LED);
}

static u8_t get_pt(u8_t* pt)
{
  /**********************************
  * Start user-specific code here. */

  // setup params

  u32_t param = 0x1;
  u32_t dst = (u32_t)pt;
  if (g_len == 16) {
    param |= 0x100;
  } else {
    param |= 0x300;
  }
  if (g_keyslot == 0) {
    param |= 0x80;
    memcpy((void *)BIGMAC_KEY, g_key, 32);
  } else if (g_dst_keyslot != 0) {
    param |= 0x10000000;
    dst = g_dst_keyslot;
  }

  BIGMAC[0] = (u32_t)pt;
  BIGMAC[1] = dst;
  BIGMAC[2] = 16;
  BIGMAC[3] = param;
  BIGMAC[4] = g_keyslot;

  //uart_puts(DEBUG_PORT, "XD\r\n");
  trigger_high();

  // start processing
  BIGMAC[7] = 1;

  while (BIGMAC[9] & 1) {}

  trigger_low();
  
  /* End user-specific code here. *
  ********************************/
  simpleserial_put('r', 16, pt);
  return 0x00;
}

static u8_t get_keyslot(u8_t* x)
{
  g_keyslot = (x[0] << 8) | x[1];
  g_dst_keyslot = (x[2] << 8) | x[3];
  g_len = x[4];
  return 0x00;
}

static u8_t reset(u8_t* x)
{
  g_keyslot = 0;
  g_len = 16;
  for (int i = 0; i < 8; i++) {
    g_key[i] = 0;
  }
  g_dst_keyslot = 0;
  return 0x00;
}

static u8_t access_mem(u8_t* x)
{
  u32_t addr = (x[0] << 24) | (x[1] << 16) | (x[2] << 8) | x[3];
  u32_t len = (x[4] << 24) | (x[5] << 16) | (x[6] << 8) | x[7];
  simpleserial_put('r', len, (void *)addr);
  return 0x00;
}

static u8_t write32(u8_t* x)
{
  u32_t addr = (x[0] << 24) | (x[1] << 16) | (x[2] << 8) | x[3];
  u32_t word = (x[4] << 24) | (x[5] << 16) | (x[6] << 8) | x[7];
  *(u32_t *)addr = word;
  return 0x00;
}

static u8_t read32(u8_t* x)
{
  u32_t addr = (x[0] << 24) | (x[1] << 16) | (x[2] << 8) | x[3];
  u32_t word;
  word = *(u32_t *)addr;
  word = ((word>>24)&0xff) | ((word<<8)&0xff0000) | ((word>>8)&0xff00) | ((word<<24)&0xff000000);
  simpleserial_put('r', 4, (u8_t *)&word);
  return 0x00;
}

static u8_t jump(u8_t* x)
{
  u32_t addr = (x[0] << 24) | (x[1] << 16) | (x[2] << 8) | x[3];
  u32_t res = ((u32_t (*)(void))addr)();
  simpleserial_put('r', 4, (u8_t *)&res);
  return 0x00;
}

static u8_t nop_loop(u8_t* x)
{
  u32_t times = (x[0] << 24) | (x[1] << 16) | (x[2] << 8) | x[3];
  trigger_high();
  __asm__ volatile ("repeat %0,end1\n"
                    "nop\n"
                    "nop\n"
                    "nop\n"
                    "nop\n"
                    "nop\n"
                    "nop\n"
                    "end1: nop\n"
                    "nop\n" :: "r" (times));
  trigger_low();
  return 0x00;
}

static u8_t div_loop(u8_t* x)
{
  u32_t times = (x[0] << 24) | (x[1] << 16) | (x[2] << 8) | x[3];
  trigger_high();
  __asm__ volatile ("movu $0, 0xffffff\n"
                    "movu $1, 0x1\n"
                    "repeat %0,end2\n"
                    "div $0, $1\n"
                    "div $0, $1\n"
                    "div $0, $1\n"
                    "div $0, $1\n"
                    "div $0, $1\n"
                    "div $0, $1\n"
                    "div $0, $1\n"
                    "div $0, $1\n"
                    "end2: nop\n"
                    "nop\n"
                    "ldc $0, $lo\n" :: "r" (times) : "$0", "$1");
  trigger_low();
  return 0x00;
}

static u8_t get_pt_sw(u8_t* pt)
{
  if (g_len == 16) {
    trigger_high();

    AES128_ECB_indp_crypto(pt);

    trigger_low();
  }
  
  simpleserial_put('r', 16, pt);
  return 0x00;
}

void main(void) {
  pervasive_clock_enable_gpio();
  pervasive_reset_exit_gpio();
  pervasive_clock_enable_uart(DEBUG_PORT);
  pervasive_reset_exit_uart(DEBUG_PORT);

  uart_init(DEBUG_PORT);

  gpio_set_port_mode(0, GPIO_PORT_GAMECARD_LED, GPIO_PORT_MODE_OUTPUT);
  gpio_set_port_mode(0, GPIO_PORT_PS_LED, GPIO_PORT_MODE_OUTPUT);
  gpio_port_set(0, GPIO_PORT_GAMECARD_LED);

  uart_puts(DEBUG_PORT, "HI\r\n");

  reset((void *)0);
  simpleserial_init();    
  simpleserial_addcmd('k', 16, get_key128);
  simpleserial_addcmd('K', 32, get_key256);
  simpleserial_addcmd('s', 5, get_keyslot);
  simpleserial_addcmd('p', 16, get_pt);
  simpleserial_addcmd('x', 0, reset);
  simpleserial_addcmd('a', 8, access_mem);
  simpleserial_addcmd('w', 8, write32);
  simpleserial_addcmd('R', 4, read32);
  simpleserial_addcmd('j', 4, jump);
  simpleserial_addcmd('l', 4, nop_loop);
  simpleserial_addcmd('L', 4, div_loop);
  simpleserial_addcmd('P', 16, get_pt_sw);
  while (1) {
    simpleserial_get();
  }
}
