#include <lebirun/lke.h>
#include <lebirun/drivers/net/ipv67.h>
#include <lebirun/drivers/net/udp.h>

void ipv67_context_destroy_all(void);
int ipv67_syscall_entry(const char *req_ptr, int unused1, int unused2);

#define IPV67_SYSCALL_NR 280

static const udp_port_hook_t ipv67_udp_hook = {
    ipv67_port_active,
    ipv67_receive_on_port,
    ipv67_receive6_on_port
};

static int ipv67_module_init(void) {
    int ret;

    ret = lke_register_syscall(IPV67_SYSCALL_NR, ipv67_syscall_entry);
    if (ret < 0) return ret;
    ret = udp_register_port_hook(&ipv67_udp_hook);
    if (ret < 0) {
        lke_unregister_syscall(IPV67_SYSCALL_NR, ipv67_syscall_entry);
        return ret;
    }
    return 0;
}

static void ipv67_module_exit(void) {
    udp_unregister_port_hook(&ipv67_udp_hook);
    lke_unregister_syscall(IPV67_SYSCALL_NR, ipv67_syscall_entry);
    ipv67_context_destroy_all();
}

LKE_NAME("ipv67");
LKE_DESC("IPv67 network stack");
LKE_LICENSE("GPLv2");
LKE_VERSION_STR("1.0.0");

module_init(ipv67_module_init);
module_exit(ipv67_module_exit);
