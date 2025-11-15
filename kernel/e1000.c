#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// Scan PCI configuration space to find E1000
static uint64
pci_find_e1000(void)
{
  // QEMU virt machine PCI ECAM base
  volatile uint32 *ecam = (volatile uint32 *)0x30000000L;
  
  // Scan bus 0, devices 0-31
  for (int dev = 0; dev < 32; dev++) {
    // PCI config space: bus 0, device dev, function 0
    // Offset in ECAM: (bus << 20) | (dev << 15) | (func << 12)
    volatile uint32 *cfg = ecam + (dev << 15) / 4;
    
    uint32 id = cfg[0];  // Vendor ID (16 bits) | Device ID (16 bits)
    
    // E1000: Vendor ID = 0x8086 (Intel), Device ID = 0x100E (82540EM)
    if (id == 0x100E8086) {
      printf("e1000: found at PCI bus 0 dev %d\n", dev);
      
      // Read BAR0 (offset 0x10 in config space)
      uint64 bar0 = cfg[0x10/4];
      
      // If BAR0 is 0 or unassigned, assign it to E1000_BASE
      if ((bar0 & ~0xF) == 0) {
        printf("e1000: BAR0 not configured, assigning 0x%lx\n", E1000_BASE);
        cfg[0x10/4] = E1000_BASE;
        bar0 = E1000_BASE;
      }
      
      // Enable bus master, memory space, and I/O space (offset 0x04 - Command register)
      cfg[0x04/4] = 0x0007;  // Bus master | Memory space | I/O space
      
      uint64 mmio_addr = bar0 & ~0xF;  // Mask off lower bits (memory type flags)
      printf("e1000: BAR0=0x%x, using MMIO at 0x%x\n", (uint32)bar0, (uint32)mmio_addr);
      return mmio_addr;
    }
  }
  
  printf("e1000: device not found on PCI bus\n");
  return E1000_BASE;  // Fallback to default
}

// called by main() to initialize the E1000 device.
void
e1000_init(void)
{
  int i;

  initlock(&e1000_lock, "e1000");

  // Find E1000 on PCI bus
  uint64 e1000_base = pci_find_e1000();
  regs = (volatile uint32 *)e1000_base;

  // Read MAC address from QEMU before reset (it's already configured by QEMU)
  uint32 ral = regs[E1000_RA];
  uint32 rah = regs[E1000_RA+1];
  printf("e1000_init: RAL=0x%x RAH=0x%x (before reset)\n", ral, rah);

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // Restore MAC address after reset (reset clears it)
  regs[E1000_RA] = ral;
  regs[E1000_RA+1] = rah;
  __sync_synchronize();  // Ensure write completes
  
  // Verify the write worked
  uint32 ral_readback = regs[E1000_RA];
  uint32 rah_readback = regs[E1000_RA+1];
  printf("e1000_init: After restore - RAL=0x%x RAH=0x%x\n", ral_readback, rah_readback);
  
  uint8 mac[6];
  e1000_get_mac(mac);
  printf("e1000_init: MAC address: %x:%x:%x:%x:%x:%x\n",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  // E1000 needs physical address for descriptor ring (32-bit only)
  uint64 tx_ring_va = (uint64)tx_ring;
  uint64 tx_ring_pa = (tx_ring_va >= KERNBASE) ? (tx_ring_va - KERNBASE) : tx_ring_va;
  regs[E1000_TDBAL] = (uint32)tx_ring_pa;  // Cast to 32-bit
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  printf("e1000_init: TX ring PA=0x%x TDT=%d TDH=%d\n", (uint32)tx_ring_pa, regs[E1000_TDT], regs[E1000_TDH]);
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    // E1000 needs physical address
    uint64 va = (uint64)rx_mbufs[i]->head;
    rx_ring[i].addr = (va >= KERNBASE) ? (va - KERNBASE) : va;
  }
  // E1000 needs physical address for descriptor ring (32-bit only)
  uint64 rx_ring_va = (uint64)rx_ring;
  uint64 rx_ring_pa = (rx_ring_va >= KERNBASE) ? (rx_ring_va - KERNBASE) : rx_ring_va;
  regs[E1000_RDBAL] = (uint32)rx_ring_pa;  // Cast to 32-bit
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_UPE |                 // unicast promiscuous (accept all unicast)
    E1000_RCTL_MPE |                 // multicast promiscuous (accept all multicast)
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(struct mbuf *m)
{
  acquire(&e1000_lock);
  
  // Get current TX tail index
  uint32 tail = regs[E1000_TDT];
  
  // Bounds check - tail should always be valid
  if (tail >= TX_RING_SIZE) {
    printf("e1000_transmit: invalid tail=%d (max=%d), TDH=%d\n", tail, TX_RING_SIZE, regs[E1000_TDH]);
    release(&e1000_lock);
    return -1;
  }
  
  // Check if descriptor is available (DD bit set means done)
  if (!(tx_ring[tail].status & E1000_TXD_STAT_DD)) {
    release(&e1000_lock);
    return -1; // Ring full
  }
  
  // Free previous mbuf if any
  if (tx_mbufs[tail])
    mbuffree(tx_mbufs[tail]);
  
  // Set up descriptor - E1000 needs physical address
  uint64 va = (uint64)m->head;
  tx_ring[tail].addr = (va >= KERNBASE) ? (va - KERNBASE) : va;
  tx_ring[tail].length = m->len;
  tx_ring[tail].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  tx_ring[tail].status = 0; // Clear DD bit
  
  // Save mbuf pointer
  tx_mbufs[tail] = m;
  
  // Advance tail
  regs[E1000_TDT] = (tail + 1) % TX_RING_SIZE;
  
  printf("e1000_transmit: sent packet len=%d, new TDT=%d\n", m->len, regs[E1000_TDT]);
  
  release(&e1000_lock);
  return 0;
}

void
e1000_recv(void)
{
  static int poll_count = 0;
  if (++poll_count % 1000 == 0) {
    // Periodic debug: check RX ring status
    uint32 tail = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
    printf("e1000_recv: RDT=%d, RDH=%d, desc[%d].status=0x%x\n", 
           regs[E1000_RDT], regs[E1000_RDH], tail, rx_ring[tail].status);
  }
  
  // Process all received packets
  while (1) {
    uint32 tail = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
    
    // Check if descriptor has a packet (DD bit set)
    if (!(rx_ring[tail].status & E1000_RXD_STAT_DD))
      break;
    
    printf("e1000_recv: packet received! len=%d\n", rx_ring[tail].length);
    
    // Get the mbuf
    struct mbuf *m = rx_mbufs[tail];
    m->len = rx_ring[tail].length;
    
    // Deliver to network stack
    net_rx(m);
    
    // Allocate new mbuf for this descriptor
    rx_mbufs[tail] = mbufalloc(0);
    if (!rx_mbufs[tail])
      panic("e1000_recv");
    // E1000 needs physical address
    uint64 va = (uint64)rx_mbufs[tail]->head;
    rx_ring[tail].addr = (va >= KERNBASE) ? (va - KERNBASE) : va;
    rx_ring[tail].status = 0; // Clear DD bit
    
    // Advance tail
    regs[E1000_RDT] = tail;
  }
}

void
e1000_intr(void)
{
  printf("e1000_intr: RX interrupt\n");
  e1000_recv();
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR];
}

void
e1000_get_mac(uint8 mac[6])
{
    uint32 ral = regs[E1000_RA];
    uint32 rah = regs[E1000_RA+1];
    
    mac[0] = (ral >> 0) & 0xFF;
    mac[1] = (ral >> 8) & 0xFF;
    mac[2] = (ral >> 16) & 0xFF;
    mac[3] = (ral >> 24) & 0xFF;
    mac[4] = (rah >> 0) & 0xFF;
    mac[5] = (rah >> 8) & 0xFF;
    
    printf("e1000_get_mac: RAL=0x%x RAH=0x%x -> [0x%x 0x%x 0x%x 0x%x 0x%x 0x%x]\n",
           ral, rah, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
