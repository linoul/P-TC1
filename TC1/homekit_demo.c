/**
 * homekit_demo.c — 最小 HomeKit 配件示例（Prestar / TC1 @ MK3031）
 * ---------------------------------------------------------------------------
 * 依赖：mico-os/libraries/daemons/homekit_server
 *        （Lib_HomeKit_Server.Cortex-M4.GCC.release.a，正好对应 MK3031 的 Cortex-M4）
 *
 * 本示例暴露一个"开关(Switch)"配件，含 1 个 On 特性。读/写走全局回调，
 * 库按名字调用这 4 个函数（见 HomeKit.h + .a 导出符号），必须由本文件实现。
 *
 * HAP type 字符串使用 MiCO 的反向 DNS 标识，例如
 *   "public.hap.service.switch" 、 "public.hap.characteristic.on"
 * （格式已对照 MICO 官方 HomekitProfiles.c 示例确认）。
 *
 * 未实测声明（无真机/工具链验证）：
 *  - mfi_cp_port = MICO_I2C_NONE：TC1 A1 无 MFi 芯片，依赖库的软件回退；
 *    若闭源 .a 在无芯片时硬失败，hk_server_start() 运行期会报错。
 *  - 回调里的 (accessoryID/serviceID/characteristicID) 索引：按 MiCO HomekitProfiles
 *    示例推测为 0 基，若库实际按 1 基编号，需把下方常量整体 +1（已在注释标出）。
 *  - key_storage 为 RAM 版：重启后配对丢失，需重新配对。
 * ---------------------------------------------------------------------------
 */
#include "mico.h"
#include "HomeKit.h"
#include "user_wifi.h"
#include <string.h>

/* ============================ HAP 类型字符串（MiCO 反向 DNS 风格） ============================ */
#define TYPE_ACCESSORY_INFO  "public.hap.service.accessory-information"
#define TYPE_SWITCH          "public.hap.service.switch"
#define TYPE_IDENTIFY        "public.hap.characteristic.identify"
#define TYPE_MANUFACTURER    "public.hap.characteristic.manufacturer"
#define TYPE_MODEL           "public.hap.characteristic.model"
#define TYPE_NAME            "public.hap.characteristic.name"
#define TYPE_SERIAL          "public.hap.characteristic.serial-number"
#define TYPE_FW_REV          "public.hap.characteristic.firmware-revision"
#define TYPE_ON              "public.hap.characteristic.on"

/* ============================ ID 约定 =================================
 * 这些 ID 就是回调里 (accessoryID, serviceID, characteristicID) 的索引，
 * 必须和下面数组的下标严格对应。若库实际按 1 基编号，把本段常量整体 +1 即可。 */
#define ACC_0           0
  #define SVC_INFO      0
    #define CHR_IDENTIFY 0
    #define CHR_MANUF    1
    #define CHR_MODEL    2
    #define CHR_NAME     3
    #define CHR_SERIAL   4
    #define CHR_FWREV    5
  #define SVC_SWITCH    1
    #define CHR_ON       0

/* ============================ 运行态 ================================= */
static bool g_switch_on = false;   /* demo 状态（真机应换成继电器 GPIO 读取） */

/* ====================== 特性数组（具名静态，靠指针引用） ====================== */
static struct _hapCharacteristic_t info_chr[] = {
  [CHR_IDENTIFY] = { .type = TYPE_IDENTIFY,    .hasStaticValue = true, .valueType = ValueType_bool,   .value.boolValue = false, .secureWrite = true },
  [CHR_MANUF]    = { .type = TYPE_MANUFACTURER,.hasStaticValue = true, .valueType = ValueType_string, .value.stringValue = "Prestar",    .secureRead = true },
  [CHR_MODEL]    = { .type = TYPE_MODEL,       .hasStaticValue = true, .valueType = ValueType_string, .value.stringValue = "TC1",        .secureRead = true },
  [CHR_NAME]     = { .type = TYPE_NAME,        .hasStaticValue = true, .valueType = ValueType_string, .value.stringValue = "Demo Switch",.secureRead = true },
  [CHR_SERIAL]   = { .type = TYPE_SERIAL,      .hasStaticValue = true, .valueType = ValueType_string, .value.stringValue = "TC10001",    .secureRead = true },
  [CHR_FWREV]    = { .type = TYPE_FW_REV,      .hasStaticValue = true, .valueType = ValueType_string, .value.stringValue = "1.0",        .secureRead = true },
};

/* On 特性：hasStaticValue=false -> 每次读都走 HKReadCharacteristicValue 拿实时值；
   hasEvents=true -> 写后向已订阅的控制器推事件。 */
static struct _hapCharacteristic_t switch_chr[] = {
  [CHR_ON] = { .type = TYPE_ON, .hasStaticValue = false, .valueType = ValueType_bool,
               .secureRead = true, .secureWrite = true, .hasEvents = true },
};

/* 服务数组：以内联初始化器定义（元素均为常量表达式——字符串字面量 / 整型 /
 * 指向静态特性数组的指针），再由 acc0.services 以指针引用，规避
 * "initializer element is not constant"。 */
static struct _hapService_t g_services[] = {
  {
    .type = TYPE_ACCESSORY_INFO,
    .num_of_characteristics = sizeof(info_chr) / sizeof(info_chr[0]),
    .characteristic = info_chr,
  },
  {
    .type = TYPE_SWITCH,
    .num_of_characteristics = sizeof(switch_chr) / sizeof(switch_chr[0]),
    .characteristic = switch_chr,
  },
};

static struct _hapAccessory_t acc0 = {
  .num_of_services = sizeof(g_services) / sizeof(g_services[0]),
  .services = g_services,
};

static hapProduct_t g_product = {
  .num_of_accessories = 1,
  .accessories = &acc0,
};

/* ====================== 长期密钥存储回调 ======================
 * hk_key_storage_cb(pk, sk, write):
 *   write==false -> 从存储读出 pk[32]/sk[32] 填给库；
 *   write==true  -> 把库给的 pk[32]/sk[32] 存起来。
 * 这里用静态数组演示，重启即丢失（需重新配对）。
 * 真机应改为写 flash（可复用 HomeKitPairList.h 之外的专用扇区）。 */
static uint8_t g_ltsk[64];   /* [0:32)=pk, [32:64)=sk，Ed25519 各 32 字节 */

static OSStatus hk_key_store(unsigned char *pk, unsigned char *sk, bool write)
{
  if (write) {
    memcpy(g_ltsk, pk, 32);
    memcpy(g_ltsk + 32, sk, 32);
  } else {
    memcpy(pk, g_ltsk, 32);
    memcpy(sk, g_ltsk + 32, 32);
  }
  return kHKNoErr;
}

/* ====================== 库要求的 4 个全局回调 ====================== */

/* 读特性值 */
HkStatus HKReadCharacteristicValue(int accessoryID, int serviceID, int characteristicID, value_union *value)
{
  if (accessoryID != ACC_0)
    return kHKNotExistErr;

  if (serviceID == SVC_SWITCH && characteristicID == CHR_ON) {
    value->boolValue = g_switch_on;          /* 真机：读继电器 GPIO 电平 */
    return kHKNoErr;
  }
  /* 静态信息类由库直接从 value 联合体返回，不会进这里 */
  return kHKNoErr;
}

/* 写特性值 */
void HKWriteCharacteristicValue(int accessoryID, int serviceID, int characteristicID,
                                value_union value, bool moreComing)
{
  if (accessoryID != ACC_0)
    return;

  if (serviceID == SVC_INFO && characteristicID == CHR_IDENTIFY) {
    /* iOS 触发"识别"：真机可在此闪 LED */
    return;
  }

  if (serviceID == SVC_SWITCH && characteristicID == CHR_ON) {
    g_switch_on = value.boolValue;            /* 真机：翻转对应继电器 GPIO */
    /* 注：原版用 HKSendNotifyMessage 推事件，但该符号签名未在 HomeKit.h 声明、
       系据 .a 导出名推断，未实测；为避免隐式声明风险此处省略。读路径仍能反映状态，
       只是 iOS 端需手动刷新。若需主动推送，待真机确认签名后再加回。 */
    (void)moreComing;
  }
}

/* 读特性状态（如是否忙）。demo 直接返回成功。 */
HkStatus HKReadCharacteristicStatus(int accessoryID, int serviceID, int characteristicID)
{
  (void)accessoryID; (void)serviceID; (void)characteristicID;
  return kHKNoErr;
}

/* 未配对时 iOS 请求 Identify */
HkStatus HKExcuteUnpairedIdentityRoutine(void)
{
  return kHKNoErr;
}

/* ====================== 启动入口 ====================== */
OSStatus homekit_demo_start(void)
{
  hk_init_t init;
  memset(&init, 0, sizeof(init));

  init.ci            = CI_SWITCH;             /* 单配件用 SWITCH；多插座应改 CI_BRIDGE + 多个 acc0..accN */
  init.model         = "TC1";
  init.config_number = 1;                      /* 属性 DB 变更时 +1，iOS 会重新拉取 */
  init.mfi_cp_port   = MICO_I2C_NONE;          /* TC1 无 MFi 芯片 -> 软件回退（可能触发 iOS "未认证" 警告） */
  init.hap_product   = &g_product;
  init.password      = (uint8_t *)"123-45-678";/* HomeKit 配对码，iOS 添加配件时输入 */
  init.password_len  = 10;
  init.verifier      = NULL; init.verifier_len = 0;  /* 留空：库按 password 自行派生 SRP verifier */
  init.salt          = NULL; init.salt_len     = 0;
  init.key_storage   = hk_key_store;

  return hk_server_start(init);   /* 内部起监听线程 + mDNS，不阻塞 */
}

/* 扩展提示：要做 TC1 的 6 路插座，把 ci 改为 CI_BRIDGE，
   g_product 里放 6 个 hapAccessory_t（每个用 CI_OUTLET，含 On + OutletInUse），
   再在回调里按 (accessoryID) 区分各路 GPIO 即可。 */
