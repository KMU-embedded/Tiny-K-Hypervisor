#include <guest_monitor.h>
#include <monitor_cli.h>
#include <log/string.h>
#include <log/uart_print.h>
#define DEBUG
#include <log/print.h>
#include <gic.h>
#include <guestloader_common.h>

struct system_map system_maps_code[18000];
uint32_t num_symbols_code;

#define MAX_LENGTH_SYMBOL 50
#define KEY_NOT_FOUND -1
int symbol_binary_search(struct system_map map[], int key, int imin, int imax)
{
    int imid;
    while (imax >= imin) {
        imid = (imin + imax) / 2;
        if (map[imid].address == key)
            return imid;
        else if (map[imid].address < key)
            imin = imid + 1;
        else
            imax = imid - 1;
    }
    if (map[imid].address > key)
        return imid - 1;
    else
        return imid;
}

/*
 * return cnt : The number of type T symbol
 */
uint32_t number_symbol(void)
{
    /* hard coding */
    char last[MAX_LENGTH_SYMBOL];
    int i;
    unsigned char *base;
    uint32_t n_symbol;
    uint32_t cnt;

    base = (unsigned char *)&system_map_start;
    n_symbol = 0;
    cnt = 0;

    while (1) {
        while (*base != ' ') {
            /* address */
            base++;
        }
        /* space */
        base++;
        /* type */
        if (*base == 't' || *base == 'T')
            cnt++;
        base++;
        /* space */
        base++;
        i = 0;
        while (*base != '\r' &&  *base != '\n') {
            /* symbol */
            last[i++] = *base++;
        }
        last[i] = '\0';
        /* carriage return */
        base++;
        n_symbol++;
        if (strcmp(last, "_end") == 0)
            break;
    }
    printh("pre cnt %d\n", cnt);
    return cnt;
}

void symbol_parser_init(void)
{
    /* hard coding */
    /* memory alloc needed to modify TODO : inkyu */
    /*
        printh("num_symbols_code is %d\n", num_symbols_code);
        struct system_map* system_maps = (struct system_map *)
            memory_alloc(n_symbol * sizeof(struct system_map));
        */
    uint8_t *base;
    int cnt;
    int cnt_code;
    int i;
    char address[9];
    char last[MAX_LENGTH_SYMBOL];

    cnt = 0;
    cnt_code = 0;
    base =  (uint8_t *)&system_map_start;

    while (1) {
        i = 0;
        while (*base != ' ') {
            /* address */
            address[i++] = *base;
            base++;
        }
        address[i] = '\0';
        system_maps_code[cnt_code].address =
            (uint32_t)arm_hexstr2uint((char *)address);
        /* space */
        base++;
        /* type */
        system_maps_code[cnt_code].type = *base;
        base++;
        /* space */
        base++;
        i = 0;
        while (*base != '\r' &&  *base != '\n') {
            /* symbol */
            system_maps_code[cnt_code].symbol[i] = last[i] = *base;
            base++;
            i++;
        }
        last[i] = '\0';
        system_maps_code[cnt_code].symbol[i] = '\0';
        /* carriage return */
        base++;
        if (strcmp(last, "_end") == 0)
            break;
        cnt++;
        if (system_maps_code[cnt_code].type == 't' ||
                system_maps_code[cnt_code].type == 'T')
            cnt_code++;
    }
}

void show_symbol(uint32_t va)
{
    int i = 0;
    for (i = 0; i < num_symbols_code; i++)
        ;
        printH("%x %c %s\n", system_maps_code[i].address,
                system_maps_code[i].type, system_maps_code[i].symbol);
}

int symbol_getter_from_va(uint32_t va, char *symbol)
{
    int i;
    i = symbol_binary_search(system_maps_code, va, 0, num_symbols_code - 1);
    if (i == KEY_NOT_FOUND) {
        printH("KEY NOT FOUND\n");
        return KEY_NOT_FOUND;
    }

    strcpy(symbol, (char *)system_maps_code[i].symbol);

    if (system_maps_code[i].address != va) {
        char add[10];
        char op[6] = " + 0x";
        arm_uint2hexstr(add, va - system_maps_code[i].address);
        strcat(symbol, op);
        strcat(symbol, add);
    }
    return 0;
}

#define ARCH_REGS_NUM_GPR    13
/* Defines the architecture specific registers */
struct arch_regs {
    uint32_t cpsr; /* CPSR */
    uint32_t pc; /* Program Counter */
    uint32_t lr;
    uint32_t gpr[ARCH_REGS_NUM_GPR]; /* R0 - R12 */
} __attribute((packed));

#define LIST 0
#define MONITORING 1
#define MEMORY 2

/* size 92 -> 0xEC00100 : memory dump, 0xEC000A0 : vmid info*/
struct monitoring_data {
    uint8_t type;
    uint32_t caller_va;
    uint32_t callee_va;
    uint32_t inst;
    uint32_t sp;
    struct arch_regs regs;
    uint32_t memory_range;
    uint32_t start_memory;
    uint8_t monitor_cnt;
};

void send_monitoring_data(uint32_t range, uint32_t src)
{
    struct monitoring_data *shared_start =
        (struct monitoring_data *)(&shared_memory_start);
    shared_start->memory_range = range;
    shared_start->start_memory = src;
}

int recovery_cnt;

void set_recovery(int cnt)
{
    recovery_cnt = cnt;
}

void monitoring_handler(int irq, void *pregs, void *pdata)
{
    char call_symbol[MAX_LENGTH_SYMBOL];
    char callee_symbol[MAX_LENGTH_SYMBOL];

    struct monitoring_data *shared_start = (struct monitoring_data
            *)(&shared_memory_start);
    int i;

    if (shared_start->type == MONITORING) {
        symbol_getter_from_va(shared_start->caller_va, call_symbol);
        symbol_getter_from_va(shared_start->callee_va, callee_symbol);

        printh("CPU 0 %s <- %s\n", call_symbol, callee_symbol);

        if (strcmp(call_symbol, "panic") == 0 && recovery_cnt == 1) {
            recovery_cnt = 0;
            printh("Target guest's panic occured!\n");
            printh("Auto system recovery start...\n");
            monitoring_reboot();
        }

    } else if (shared_start->type == LIST) {
        uint32_t *dump_base = (uint32_t *)(&shared_memory_start) + (0x100/4);
        uint32_t va, inst;
        for (i = 0; i < shared_start->monitor_cnt; i++) {
            va = *dump_base;
            symbol_getter_from_va(va, call_symbol);
            dump_base++;
            inst = dump_base;
            /* showing list */
            printh("Monitoring symbol is %s, va is %x, instruction is %x\n",
                     call_symbol, va, inst);
        }
    } else if (shared_start->type == MEMORY) {
        /* dump memory */
        int i, j;
        uint32_t *dump_base = (uint32_t *)(&shared_memory_start) + (0x100/4);
        uint32_t base_memory = shared_start->start_memory;
        for (i = 0; i < shared_start->memory_range; i++) {
            if ((uint32_t)dump_base > (uint32_t)&shared_memory_end) {
                printh("The memory range out!!\n");
                return;
            }
            if (i % 4 == 0)
                printH("0x%x :", base_memory);
            printh(" 0x%x", *dump_base);
            dump_base++;
            if (i % 4 == 3) {
                printh("\n");
                base_memory += 16;
            }
        }
        printh("\n");
    }
}

void allset(void)
{
    int i = 0;
    for (i = 0; i < num_symbols_code; i++)
        monitoring_allset(system_maps_code[i].address);
}

#define MONITORING_IRQ 20

void reboot(void)
{
    copy_image_to(&restore_start, &restore_end, &loader_start);

    uart_print("\nGuest Linux reboot!!\n");
    loader_boot_guest(GUEST_TYPE);
}

void monitoring_init(void)
{
#ifdef BM_GUEST
    gic_set_irq_handler(MONITORING_IRQ, monitoring_handler, 0);
    num_symbols_code =  number_symbol();
    symbol_parser_init();
#endif

#ifdef LINUX_GUEST
    /* For reboot */
    /* TODO Not arndale board yet */
    copy_image_to(&loader_start, &guestloader_end, &restore_start);
#endif
}

