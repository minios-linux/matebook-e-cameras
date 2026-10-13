// SPDX-License-Identifier: GPL-2.0
#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/pci.h>

static bool reset_graph;
module_param(reset_graph, bool, 0600);

static acpi_status dump_sensor(acpi_handle handle, u32 level, void *context,
			      void **return_value)
{
	const char *methods[] = { "_DDN", "SSDB" };
	unsigned int i;

	if (reset_graph)
		acpi_fwnode_handle(acpi_fetch_acpi_dev(handle))->secondary = NULL;

	for (i = 0; i < ARRAY_SIZE(methods); i++) {
		struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
		union acpi_object *obj;

		if (ACPI_FAILURE(acpi_evaluate_object(handle, (char *)methods[i],
						     NULL, &buffer)))
			continue;
		obj = buffer.pointer;
		if (obj->type == ACPI_TYPE_STRING)
			pr_info("matebook-acpi: %s %s: %s\n", (char *)context,
				methods[i], obj->string.pointer);
		if (obj->type == ACPI_TYPE_BUFFER)
			print_hex_dump(KERN_INFO, "matebook-acpi SSDB: ",
				       DUMP_PREFIX_OFFSET, 16, 1, obj->buffer.pointer,
				       obj->buffer.length, false);
		kfree(buffer.pointer);
	}
	return AE_OK;
}

static int __init matebook_acpi_init(void)
{
	struct pci_dev *pdev;

	if (reset_graph) {
		pdev = pci_get_device(PCI_VENDOR_ID_INTEL, 0x465d, NULL);
		if (pdev) {
			set_secondary_fwnode(&pdev->dev, NULL);
			pci_dev_put(pdev);
		}
	}
	acpi_get_devices("HYV0846", dump_sensor, "HYV0846", NULL);
	acpi_get_devices("SSE5364", dump_sensor, "SSE5364", NULL);
	return 0;
}

static void __exit matebook_acpi_exit(void) {}
module_init(matebook_acpi_init);
module_exit(matebook_acpi_exit);
MODULE_LICENSE("GPL");
