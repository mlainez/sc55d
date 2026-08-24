import re, sys
p = sys.argv[1]
s = open(p).read()
i = s.index('uint8_t TIMER_Read(mcu_timer_t& timer, uint32_t address)')
j = s.index('TIMER_Sync(timer, timer.cycles);', i)
s = s[:j] + '(void)0; // MUTANT: no sync' + s[j+len('TIMER_Sync(timer, timer.cycles);'):]
open(p, 'w').write(s)
