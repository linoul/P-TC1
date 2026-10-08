/**
 * homekit_demo.c — 最小 HomeKit 配件示例（Prestar / TC1 @ MK3031）
 * ---------------------------------------------------------------------------
 * 依赖：mico-os/libraries/daemons/homekit_server
 *        （Lib_HomeKit_Server.Cortex-M4.GCC.release.a，正好对应 MK3031 的 Cortex-M4）
 *
 * 本示例暴露一个"开关(Switch)"配件，含 1 个 On 特性。读/写走全局回调，
 * 库按名字调用这 4 个函数（见 HomeKit.h + .a 导出符号），必须由本文件实现。
 *
 * 未实测声明：本文件未在真机/工具链上编译运行（本机无 MiCO 交叉工具链）。
 * 代码严格依据 HomeKit.h 数据结构与 .a 导出符号编写；其中 HKSendNotifyMessage
 * 的签名是依据导出符号名推断的，若编译报隐式声明或运行异常，把对应调用注释掉即可
 * （读路径仍能反映状态，只是 iOS 端不会自动刷新）。
 * ---------------------------------------------------------------------------
 */
#include "HomeKit.h"
#include <string.h>

/* ============================ HAP 短 UUID ============================ */
/* 基 UUID: 00000000-0000-1000-8000-0026BB765291，下面用短形式 */
#define TYPE_ACCESSORY_INFO  "3E"   /* Accessory Information 服务 */
#define TYPE_SWITCH          "49"   /* Switch 服务 */
#define TYPE_IDENTIFY        "14"   /* Identify 特性（写） */
#define TYPE_MANUFACTURER    "20"
#define TYPE_MODEL           "21"
#define TYPE_NAME            "23"
#define TYPE_SERIAL          "30"
#define TYPE_FW_REV          "52"
#define TYPE_ON              "25"   /* On 特性（bool） */

/* ============================ ID 约定 ================================= */
/* 这些 ID 就是回调里 (accessoryID, serviceID, characteristicID) 的索引，
   必须和下面数组的下标严格对应。 */
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

/* ====================== 配件属性树（attribute DB） ====================== */
static struct _hapCharacteristic_t info_chr[] = {
  [CHR_IDENTIFY] = { .type = TYPE_IDENTIFY,    .hasStaticValue = true, .valueType = ValueType_bool,   .value.boolValue = false, .secureWrite = true },
  [CHR_MANUF]    = { .type = TYPE_MANUFACTURER,.hasStaticValue = true, .valueType = ValueType_string, .value.stringValue = "Prestar",    .secureRead = true },
  [CHR_MODEL]    = { .type = TYPE_MODEL,       .hasStaticValue = true, .valueType = ValueType_string, .value.stringValue = "TC1",        .secureRead = true },
  [CHR_NAME]     = { .type = TYPE_NAME,        .hasStaticValue = true, .valueType = ValueType_string, .value.stringValue = "Demo Switch",.secureRead = true },
  [CHR_SERIAL]   = { .type = TYPE_SERIAL,      .hasStaticValue = true, .valueType = ValueType_string, .value.stringValue = "TC10001",    .secureRead = true },
  [CHR_FWREV]    = { .type = TYPE_FW_REV,      .hasStaticValue = true, .valueType = ValueType_string, .value.stringValue = "1.0",        .secureRead = true },
};

static struct _hapService_t info_svc = {
  .type = TYPE_ACCESSORY_INFO,
  .num_of_characteristics = sizeof(info_chr) / sizeof(info_chr[0]),
  .characteristic = info_chr,
};

/* On 特性：hasStaticValue=false -> 每次读都走 HKReadCharacteristicValue 拿实时值；
   hasEvents=true -> 写后向已订阅的控制器推事件。 */
static struct _hapCharacteristic_t switch_chr[] = {
  [CHR_ON] = { .type = TYPE_ON, .hasStaticValue = false, .valueType = ValueType_bool,
               .secureRead = true, .secureWrite = true, .hasEvents = true },
};

static struct _hapService_t switch_svc = {
  .type = TYPE_SWITCH,
  .num_of_characteristics = sizeof(switch_chr) / sizeof(switch_chr[0]),
  .characteristic = switch_chr,
};

static struct _hapAccessory_t acc0 = {
  .num_of_services = 2,
  .services = (struct _hapService_t[]){ info_svc, switch_svc },
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
    /* moreComing==false 时表示本次写事务结束，可推事件 */
    if (!moreComing) {
      /* HKSendNotifyMessage 签名依据 .a 导出符号推断，未从源码确认；
         若编译/运行异常，注释掉本调用即可（读路径仍能反映状态）。 */
      extern OSStatus HKSendNotifyMessage(int, int, int, value_union);
      HKSendNotifyMessage(ACC_0, SVC_SWITCH, CHR_ON, value);
    }
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
