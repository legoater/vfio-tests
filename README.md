# VFIO Test Suite

User-space test programs for the Linux VFIO and IOMMUFD subsystems.
Exercises PCI device open, DMA mapping, hot reset, hugepage fault
handling, and live migration through the VFIO kernel interfaces.

## Building

    make

## Running

Tests require root and a PCI device bound to a VFIO driver
(`vfio-pci`).  Bind the device, then:

    make check DEVICE=0000:08:10.0

or directly:

    ./run-test.sh 0000:08:10.0

The runner allocates 2 MB hugepages, detects IOMMU limits, and reports
a pass/fail/skip summary.

Set `VFIO_VERBOSE=1` for detailed output.

## Tests

| Test | Description |
|------|-------------|
| `vfio-pci-device-open` | Open device, enumerate regions and IRQs (legacy group) |
| `vfio-pci-device-open-sparse-mmap` | Open device, test sparse mmap regions (legacy group) |
| `vfio-pci-device-open-igd` | Open Intel IGD device, read OpRegion (legacy group) |
| `vfio-pci-vf-token`  | Open a VF using a VF token (legacy group) |
| `vfio-noiommu-pci-device-open` | Open device in no-IOMMU mode (legacy group) |
| `iommufd-pci-device-open` | Open device, enumerate regions, DMA map, hot reset (iommufd) |
| `iommufd-dmabuf` | VFIO dma-buf BAR export and P2P mapping via iommufd |
| `iommufd-dma-map-unmap` | DMA map/unmap stress test with 4KB chunks (iommufd) |
| `iommufd-pci-vf-token`  | Open a VF using a VF token (iommufd) |
| `vfio-pci-device-dma-map` | DMA map BAR regions and test high/low memory mappings |
| `vfio-pci-device-map-alignment` | Test BAR mmap alignment at various power-of-2 sizes |
| `vfio-pci-bar-fault-timing` | Measure BAR mmap page fault latency at PTE/PMD/PUD sizes |
| `vfio-correctness-tests` | DMA correctness tests with hugepage backing |
| `vfio-iommu-map-unmap` | DMA map/unmap stress test with 4KB chunks (legacy container) |
| `vfio-iommu-stress-test` | Interleaved 2MB DMA mapping stress test (legacy container) |
| `vfio-huge-guest-test` | Hugepage guest memory DMA mapping test |
| `vfio-pci-huge-fault-race` | Race test for huge page BAR mmap faults (legacy group) |
| `vfio-pci-hot-reset` | PCI hot reset via VFIO (legacy group) |
| `vfio-pci-device-reset` | PCI device reset (FLR) via VFIO (legacy group) |
| `vfio-pci-device-irq-setup` | PCI IRQ setup test: INTx, MSI, MSI-X, ERR, REQ (legacy group) |
| `vfio-pci-device-config-access` | PCI config space access: header, capabilities, write/readback |
| `vfio-pci-device-migration` | Query device migration and dirty tracking capabilities |
| `vfio-pci-device-migration-stress` | Migration state cycle stress test (iommufd) |
| `igb-tx-rx` | Intel 82576/i350 (igb) TX/RX DMA test (iommufd) |
| `iova-stress.sh` | IOVA stress test across large address ranges (default 45TB) |

## Tools

### vfio-check-alignment

This tool verifies VFIO BAR mmap alignment.

To run the script, you need to start a domain with a VFIO device and pass the domain memory 
map information to the script. Example:

`Assuming PID as QEMU domain's process id.`

    cat /proc/{PID}/maps | grep vfio | ./vfio-check-alignment.py

## License

GPLv2 -- see `COPYING`.
