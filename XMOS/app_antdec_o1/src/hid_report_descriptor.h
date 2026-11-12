#ifndef __hid_report_descriptor_h__
#define __hid_report_descriptor_h__

#include "xua_hid_report.h"

static const USB_HID_Short_Item_t hidCollectionApplication = {
    .header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_MAIN, HID_REPORT_ITEM_TAG_COLLECTION),
    .data = { 0x01, 0x00 }
};
static const USB_HID_Short_Item_t hidCollectionEnd = {
    .header = HID_REPORT_SET_HEADER(0, HID_REPORT_ITEM_TYPE_MAIN, HID_REPORT_ITEM_TAG_END_COLLECTION),
    .data = { 0x00, 0x00 }
};
static const USB_HID_Short_Item_t hidInputConstArray = {
    .header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_MAIN, HID_REPORT_ITEM_TAG_INPUT),
    .data = { 0x01, 0x00 }
};
static const USB_HID_Short_Item_t hidInputDataVar = {
    .header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_MAIN, HID_REPORT_ITEM_TAG_INPUT),
    .data = { 0x02, 0x00 }
};
static const USB_HID_Short_Item_t hidLogicalMaximum0 = {
    .header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_GLOBAL, HID_REPORT_ITEM_TAG_LOGICAL_MAXIMUM),
    .data = { 0x00, 0x00 }
};
static const USB_HID_Short_Item_t hidLogicalMaximum1 = {
    .header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_GLOBAL, HID_REPORT_ITEM_TAG_LOGICAL_MAXIMUM),
    .data = { 0x01, 0x00 }
};
static const USB_HID_Short_Item_t hidLogicalMinimum0 = {
    .header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_GLOBAL, HID_REPORT_ITEM_TAG_LOGICAL_MINIMUM),
    .data = { 0x00, 0x00 }
};
static const USB_HID_Short_Item_t hidReportCount2 = {
    .header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_GLOBAL, HID_REPORT_ITEM_TAG_REPORT_COUNT),
    .data = { 0x02, 0x00 }
};
static const USB_HID_Short_Item_t hidReportCount1 = {
    .header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_GLOBAL, HID_REPORT_ITEM_TAG_REPORT_COUNT),
    .data = { 0x01, 0x00 }
};
static const USB_HID_Short_Item_t hidReportSize1 = {
    .header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_GLOBAL, HID_REPORT_ITEM_TAG_REPORT_SIZE),
    .data = { 0x01, 0x00 }
};
static const USB_HID_Short_Item_t hidUsageConsumerControl = {
    .header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_LOCAL, HID_REPORT_ITEM_TAG_USAGE),
    .data = { 0x01, 0x00 }
};

static const USB_HID_Report_Element_t hidReportPageConsumer = {
    .item.header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_GLOBAL, HID_REPORT_ITEM_TAG_USAGE_PAGE),
    .item.data = { USB_HID_USAGE_PAGE_ID_CONSUMER, 0x00 },
    .location = HID_REPORT_SET_LOC(0, 1, 0, 0)
};

/* Bit 0: Play/Pause (0xCD), Bit 1: Next (0xB5), Bit 2: Prev (0xB6),
 * Bit 3: Vol+ (0xE9), Bit 4: Vol- (0xEA), Bit 5: Mute (0xE2) */
static USB_HID_Report_Element_t hidUsageByte0Bit5 = {
    .item.header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_LOCAL, HID_REPORT_ITEM_TAG_USAGE),
    .item.data = { 0xE2, 0x00 },
    .location = HID_REPORT_SET_LOC(0, 0, 0, 5)
};
static USB_HID_Report_Element_t hidUsageByte0Bit4 = {
    .item.header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_LOCAL, HID_REPORT_ITEM_TAG_USAGE),
    .item.data = { 0xEA, 0x00 },
    .location = HID_REPORT_SET_LOC(0, 0, 0, 4)
};
static USB_HID_Report_Element_t hidUsageByte0Bit3 = {
    .item.header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_LOCAL, HID_REPORT_ITEM_TAG_USAGE),
    .item.data = { 0xE9, 0x00 },
    .location = HID_REPORT_SET_LOC(0, 0, 0, 3)
};
static USB_HID_Report_Element_t hidUsageByte0Bit2 = {
    .item.header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_LOCAL, HID_REPORT_ITEM_TAG_USAGE),
    .item.data = { 0xB6, 0x00 },
    .location = HID_REPORT_SET_LOC(0, 0, 0, 2)
};
static USB_HID_Report_Element_t hidUsageByte0Bit1 = {
    .item.header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_LOCAL, HID_REPORT_ITEM_TAG_USAGE),
    .item.data = { 0xB5, 0x00 },
    .location = HID_REPORT_SET_LOC(0, 0, 0, 1)
};
static USB_HID_Report_Element_t hidUsageByte0Bit0 = {
    .item.header = HID_REPORT_SET_HEADER(1, HID_REPORT_ITEM_TYPE_LOCAL, HID_REPORT_ITEM_TAG_USAGE),
    .item.data = { 0xCD, 0x00 },
    .location = HID_REPORT_SET_LOC(0, 0, 0, 0)
};

static USB_HID_Report_Element_t * const hidConfigurableElements[] = {
    &hidUsageByte0Bit0,
    &hidUsageByte0Bit1,
    &hidUsageByte0Bit2,
    &hidUsageByte0Bit3,
    &hidUsageByte0Bit4,
    &hidUsageByte0Bit5
};

static const USB_HID_Report_Element_t * const hidReports[] = {
    &hidReportPageConsumer
};

static const USB_HID_Short_Item_t * const hidReportDescriptorItems[] = {
    &(hidReportPageConsumer.item),
    &hidUsageConsumerControl,
    &hidCollectionApplication,
        &hidLogicalMinimum0,
        &hidLogicalMaximum1,
        &hidReportSize1,
        &hidReportCount1,
        &(hidUsageByte0Bit0.item),
        &hidInputDataVar,
        &(hidUsageByte0Bit1.item),
        &hidInputDataVar,
        &(hidUsageByte0Bit2.item),
        &hidInputDataVar,
        &(hidUsageByte0Bit3.item),
        &hidInputDataVar,
        &(hidUsageByte0Bit4.item),
        &hidInputDataVar,
        &(hidUsageByte0Bit5.item),
        &hidInputDataVar,
        &hidLogicalMaximum0,
        &hidReportCount2,
        &hidInputConstArray,
    &hidCollectionEnd
};

#define HID_REPORT_COUNT (1)

#endif /* __hid_report_descriptor_h__ */
