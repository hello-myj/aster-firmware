"""Force the actual Zephyr USB timer into the writable-check/write gap."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVER = ROOT.parent.parent / 'zephyr/drivers/serial/serial_esp32_usb.c'


def function(text, name):
    start = text.index('static void '+name+'(')
    opening = text.index('{', start)
    depth = 1
    end = opening+1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


@unittest.skipUnless(DRIVER.exists(), 'Zephyr checkout is optional for SDK-only host jobs')
class UsbSerialTxTests(unittest.TestCase):
    def test_timer_cannot_flush_between_check_and_write(self):
        text = DRIVER.read_text()
        self.assertIn('struct k_spinlock tx_lock;', text,
                      'Apply the tracked USB serial batching patch to Zephyr first')
        start = text.index('struct serial_esp32_usb_data {')
        struct = text[start:text.index('\n};', start)+3]
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
struct k_spinlock { int locked; };
struct k_timer { int unused; };
typedef int k_spinlock_key_t;
struct device {void *data;};
#define CONTAINER_OF(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define K_USEC(n) (n)
#define K_NO_WAIT 0
#define USBSERIAL_TAIL_FLUSH_DELAY_US 250
#define USBSERIAL_FLUSH_RETRY_LIMIT 20
#define USBSERIAL_POLL_OUT_TIMEOUT_MS 50
static struct k_timer *injected_timer;
static int lock_depth, inject, pending, writable, writes, dropped, preempt_wait;
static int64_t fake_now;
static void serial_esp32_usb_flush_timer_expiry(struct k_timer *timer);
static int k_spin_lock(struct k_spinlock *s) {
 assert(!s->locked);s->locked=1;++lock_depth;return 0;
}
static void k_spin_unlock(struct k_spinlock *s,int key) {
 (void)key;assert(s->locked);s->locked=0;--lock_depth;
 if(preempt_wait&&!writable){preempt_wait=0;fake_now+=100;writable=1;}
 if(!lock_depth&&pending){pending=0;serial_esp32_usb_flush_timer_expiry(injected_timer);}
}
static bool usb_serial_jtag_ll_txfifo_writable(void) {
 bool ready=writable;
 if(inject){inject=0;if(lock_depth)pending=1;else serial_esp32_usb_flush_timer_expiry(injected_timer);}
 return ready;
}
static void usb_serial_jtag_ll_txfifo_flush(void) {writable=0;}
static void usb_serial_jtag_ll_write_txfifo(unsigned char *c,int n) {
 (void)c;assert(n==1);if(writable)++writes;else ++dropped;
}
static void k_timer_start(struct k_timer *t,int delay,int period) {(void)t;(void)delay;(void)period;}
static void k_timer_stop(struct k_timer *t) {(void)t;}
static int64_t k_uptime_get(void) {return fake_now;}
static int k_is_in_isr(void) {return 0;}
'''+struct+'\n'+function(text,'serial_esp32_usb_flush_timer_expiry')+'\n'+function(text,'serial_esp32_usb_poll_out')+r'''
int main(void) {
 struct serial_esp32_usb_data data={0};struct device dev={.data=&data};
 injected_timer=&data.tx_flush_timer;
 for(int n=0;n<10000;++n){writable=1;inject=1;serial_esp32_usb_poll_out(&dev,'a');}
 assert(writes==10000&&dropped==0&&!lock_depth&&!pending);
 /* A high-priority render preemption lasts longer than the wall timeout,
  * but the connected host drains the FIFO while the shell is off CPU. */
 data.last_tx_time=fake_now;writable=0;preempt_wait=1;
 serial_esp32_usb_poll_out(&dev,'b');
 assert(writes==10001&&dropped==0&&!preempt_wait);
 puts("10000 forced timer interleavings, no lost bytes");
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src = pathlib.Path(folder)/'serial-tx.c'
            src.write_text(code)
            binary = str(src.with_suffix(''))
            build = subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                                    '-fsanitize=address,undefined',str(src),'-o',binary],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('no lost bytes',result.stdout)
