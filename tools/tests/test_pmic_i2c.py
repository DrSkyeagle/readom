#!/usr/bin/env python3
"""Compile the three actual PMIC transfer helpers with a controlled I2C stub."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
#define I2C_M_TEN 16
#define I2C_M_RD 1
#define LOG_READ 1
#define LOG_WRITE 2
struct i2c_adapter { int unused; };
struct i2c_client { struct i2c_adapter *adapter; int addr; int flags; };
struct max77696_i2c { struct i2c_client *client; };
struct i2c_msg { unsigned addr, flags, len; char *buf; };
static int result, calls, logs, expected_messages, expected_payload_len;
static int logs_at_transfer, checks;
static int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msg, int count) {
    assert(adap != NULL); assert(count == expected_messages);
    assert(msg[0].addr == 0x3c && msg[0].buf[0] == 0x20);
    logs_at_transfer = logs; calls++;
    if (count == 2) {
        assert(msg[0].len == 1 && msg[0].flags == I2C_M_TEN);
        assert(msg[1].flags == (I2C_M_TEN | I2C_M_RD));
        assert(msg[1].len == (unsigned)expected_payload_len);
        if (result == 2) memset(msg[1].buf, 0x6a, msg[1].len);
    } else {
        assert(msg[0].len == (unsigned)expected_payload_len + 1);
        assert(msg[0].flags == I2C_M_TEN);
        for (unsigned i=1; i<msg[0].len; i++) assert((u8)msg[0].buf[i] == 0x6a);
    }
    return result;
}
static void log_i2c(int action, int slave, int reg, int value) {
    assert(action == LOG_READ || action == LOG_WRITE);
    assert(slave == 0x3c && reg >= 0x20 && reg <= 0x22);
    (void)value; logs++;
}
'''
SUFFIX = r'''
int main(void) {
    struct i2c_adapter adapter = {0};
    struct i2c_client client = {&adapter, 0x3c, I2C_M_TEN};
    struct max77696_i2c chip = {&client};
    const int results[] = {-ETIMEDOUT, -EBUSY, -EIO, 0, 1, 2, 3};
    for (int operation=0; operation<3; operation++) {
        for (unsigned j=0; j<sizeof(results)/sizeof(results[0]); j++) {
            u8 data[3] = {0x6a,0x6a,0x6a};
            result=results[j]; calls=logs=logs_at_transfer=0;
            expected_messages = operation == 0 ? 2 : 1;
            expected_payload_len = operation == 2 ? 1 : 3;
            int rc = operation == 0 ? max77696_i2c_seq_read(&chip,0x20,data,3) :
                operation == 1 ? max77696_i2c_seq_write(&chip,0x20,data,3) :
                max77696_i2c_single_write(&chip,0x20,0x6a);
#ifdef EXPECT_STRICT
            int expected = result == expected_messages ? 0 : result < 0 ? result : -EIO;
            assert(logs == (result == expected_messages ? expected_payload_len : 0));
            assert(logs_at_transfer == 0);
#else
            int expected = result == expected_messages ? 0 : result;
            assert(logs == expected_payload_len);
            assert(logs_at_transfer == (operation == 2 ? 1 : 0));
#endif
            assert(rc == expected); assert(calls == 1); checks++;
        }
    }
    printf("PASS: %d transfer-result cases, actual helper bodies, simulated bus only\n",checks);
    return 0;
}
'''


def extract(text, name):
    start = text.index('static int ' + name + ' ')
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('receipt', type=Path)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    names = ['max77696_i2c_seq_read', 'max77696_i2c_seq_write', 'max77696_i2c_single_write']
    bodies = {label: '\n\n'.join(extract(path.read_text(), name) for name in names)
              for label, path in [('before',args.before),('after',args.after)]}
    results = []
    with tempfile.TemporaryDirectory(prefix='pw3-pmic-helpers-') as temporary:
        root=Path(temporary)
        for label, strict in [('before',False),('after',False),('after',True)]:
            case=label + ('-strict' if strict else '-legacy')
            source=root/(case+'.c'); exe=root/case
            source.write_text(PREFIX + bodies[label] + SUFFIX)
            command=[args.cc,'-std=c99','-Wall','-Wextra','-Werror','-O1','-g',
                     '-fsanitize=address,undefined','-fno-omit-frame-pointer']
            if strict: command += ['-DCONFIG_LAB126_ANDROID_PMIC_I2C_STRICT','-DEXPECT_STRICT']
            command += [str(source),'-o',str(exe)]
            subprocess.run(command,check=True,text=True)
            run=subprocess.run([str(exe)],check=True,capture_output=True,text=True)
            results.append({'case':case,'cases':21,'stdout':run.stdout.strip(),
                            'extracted_helper_sha256':hashlib.sha256(bodies[label].encode()).hexdigest()})
    receipt={'scope':'Host function-level test; no real I2C bus, ARM runtime or device writes',
             'sources':{label:hashlib.sha256(path.read_bytes()).hexdigest()
                        for label,path in [('before',args.before),('after',args.after)]},
             'compiler':subprocess.check_output([args.cc,'--version'],text=True).splitlines()[0],
             'asan_ubsan':True,'results':results}
    args.receipt.parent.mkdir(parents=True,exist_ok=True)
    args.receipt.write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))

if __name__ == '__main__':
    main()
