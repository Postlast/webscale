/******************************************************************************
 * FileName: web_int_callbacks.c
 * Description: The web server inernal callbacks.
*******************************************************************************/

#include "user_config.h"
#include "bios.h"
#include "add_sdk_func.h"
#include "hw/esp8266.h"
#include "hw/uart_register.h"
#include "ets_sys.h"
#include "os_type.h"
#include "osapi.h"
#include "user_interface.h"
#include "lwip/tcp.h"
#include "flash.h"
#include "flash_eep.h"
#include "tcp_srv_conn.h"
#include "web_srv_int.h"
#include "wifi.h"
#include "web_utils.h"
#include "driver/adc.h"
#include "webfs.h"
#include "tcp2uart.h"
#ifdef USE_NETBIOS
#include "netbios.h"
#endif
#ifdef USE_SNTP
#include "sntp.h"
#endif
#include "rom2ram.h"
#include "sys_const.h"

extern TCP_SERV_CONN * tcp2uart_conn;
/*extern uint32 adc_rand_noise;
extern uint32 dpd_bypass_original;
extern uint16 phy_freq_offset
extern uint8 phy_in_most_power;
extern uint8 rtc_mem_check_fail;
extern uint8 phy_set_most_tpw_disbg;
extern uint8 phy_set_most_tpw_index; // uint32? */
extern uint8 phy_in_vdd33_offset;

#define mMIN(a, b)  ((a<b)?a:b)
//-------------------------------------------------------------------------------
// Test adc
// Читает adc в одиночный буфер (~2килобайта) на ~20ksps и сохраняет в виде WAV
// Правильное чтение организуется по прерыванию таймера(!).
// Тут только демо!
//-------------------------------------------------------------------------------
typedef struct
{ // https://ccrma.stanford.edu/courses/422/projects/WaveFormat/
  unsigned long int RIFF ;/* +00 'RIFF'           */
  unsigned long int size8;/* +04 file size - 8    */
  unsigned long int WAVE ;/* +08 'WAVE'           */
  unsigned long int fmt  ;/* +12 'fmt '           */
  unsigned long int fsize;/* +16 указатель до 'fact' или 'data' */
  unsigned short int ccod;/* +20 01 00  Compression code: 1 - PCM/uncompressed */
  unsigned short int mono;/* +22 00 01 или 00 02  */
  unsigned long int freq ;/* +24 частота          */
  unsigned long int bps  ;/* +28                  */
  unsigned short int blka;/* +32 1/2/4  BlockAlign*/
  unsigned short int bits;/* +34 разрядность 8/16 */
  unsigned long int data ;/* +36 'data'           */
  unsigned long int dsize;/* +40 размер данных    */
} WAV_HEADER;
const WAV_HEADER ICACHE_RODATA_ATTR wav_header =
{0x46464952L,
 0x00000008L,
 0x45564157L,
 0x20746d66L,
 0x00000010L,
 0x0001     ,
 0x0001     ,
 0x000055f0L,
 0x000055f0L,
 0x0002     ,
 0x0010     ,
 0x61746164L,
 0x00000000L};
#define WAV_HEADER_SIZE sizeof(wav_header)

void ICACHE_FLASH_ATTR web_test_adc(TCP_SERV_CONN *ts_conn)
{
	WEB_SRV_CONN *web_conn = (WEB_SRV_CONN *) ts_conn->linkd;
    unsigned int len = web_conn->msgbufsize - web_conn->msgbuflen;
    if(len > WAV_HEADER_SIZE + 10) {
    	len -= WAV_HEADER_SIZE;
    	WAV_HEADER * ptr = (WAV_HEADER *) &web_conn->msgbuf[web_conn->msgbuflen];
    	os_memcpy(ptr, &wav_header, WAV_HEADER_SIZE);
    	ptr->dsize = len;
    	web_conn->msgbuflen += WAV_HEADER_SIZE;
    	len >>= 1;
    	read_adcs((uint16 *)(web_conn->msgbuf + web_conn->msgbuflen), len);
    	web_conn->msgbuflen += len << 1;
    }
    SetSCB(SCB_FCLOSE | SCB_DISCONNECT); // connection close
}
//===============================================================================
// WiFi Scan XML
//-------------------------------------------------------------------------------
void ICACHE_FLASH_ATTR web_wscan_xml(TCP_SERV_CONN *ts_conn)
{
	struct bss_scan_info si;
	WEB_SRV_CONN *web_conn = (WEB_SRV_CONN *) ts_conn->linkd;
    // Check if this is a first round call
    if(CheckSCB(SCB_RETRYCB)==0) {
    	tcp_puts_fd("<total>%d</total>", total_scan_infos);
    	if(total_scan_infos == 0) return;
    	web_conn->udata_start = 0;
    }
	while(web_conn->msgbuflen + 96 + 32 <= web_conn->msgbufsize) {
	    if(web_conn->udata_start < total_scan_infos) {
	    	struct bss_scan_info *p = (struct bss_scan_info *)buf_scan_infos;
	    	p += web_conn->udata_start;
	    	ets_memcpy(&si, p, sizeof(si));
/*
	    	uint8 ssid[33];
	    	ssid[32] = '\0';
	    	ets_memcpy(ssid, si.ssid, 32); */
	    	uint8 ssid[32*6 + 1];
	    	if(web_conn->msgbuflen + 96 + htmlcode(ssid, si.ssid, 32*6, 32) > web_conn->msgbufsize) break;
			tcp_puts_fd("<ap id=\"%d\"><ch>%d</ch><au>%d</au><bs>" MACSTR "</bs><ss>%s</ss><rs>%d</rs><hd>%d</hd></ap>", web_conn->udata_start, si.channel, si.authmode, MAC2STR(si.bssid), ssid, si.rssi, si.is_hidden);
	   		web_conn->udata_start++;
	    	if(web_conn->udata_start >= total_scan_infos) {
			    ClrSCB(SCB_RETRYCB);
			    return;
	    	}
	    }
	    else {
		    ClrSCB(SCB_RETRYCB);
		    return;
	    }
	}
	// repeat in the next call ...
    SetSCB(SCB_RETRYCB);
    SetNextFunSCB(web_wscan_xml);
    return;
}
//===============================================================================
// RAM hexdump
//-------------------------------------------------------------------------------
void ICACHE_FLASH_ATTR web_hexdump(TCP_SERV_CONN *ts_conn)
{
	WEB_SRV_CONN *web_conn = (WEB_SRV_CONN *) ts_conn->linkd;
	union {
		uint32 dw[4];
		uint8 b[16];
	}data;
	web_conn->udata_start &= 0xfffffff0;
	uint32 *addr = (uint32 *)web_conn->udata_start;
	int i;
	web_conn->udata_stop &= 0xfffffff0;
	while(web_conn->msgbuflen + (9+3*16+17+2) <= web_conn->msgbufsize) {
		if(((uint32)addr >= 0x20000000)&&(((uint32)addr < 0x60002000)||((uint32)addr >= 0x60008000))) {
			tcp_puts("%p:", addr);
			for(i=0 ; i < 4 ; i++) data.dw[i] = *addr++;
			web_conn->udata_start = (uint32)addr;
			if(ts_conn->flag.user_option1) {
				for(i=0 ; i < 4 ; i++) tcp_puts(" %08x", data.dw[i]);
			}
			else {
				for(i=0 ; i < 16 ; i++) tcp_puts(" %02x", data.b[i]);
			}
			tcp_put(' '); tcp_put(' ');
			for(i=0 ; i < 16 ; i++) tcp_put((data.b[i] >=' ' && data.b[i] != 0x7F)? data.b[i] : '.');
			tcp_puts("\r\n");
			if((uint32)addr >= web_conn->udata_stop) {
			    ClrSCB(SCB_RETRYCB);
			    SetSCB(SCB_FCLOSE | SCB_DISCONNECT); // connection close
			    return;
			}
		}
		else {
			tcp_puts("%p = Bad address!\r\n", addr);
		    ClrSCB(SCB_RETRYCB);
		    SetSCB(SCB_FCLOSE | SCB_DISCONNECT); // connection close
		    return;
		}
	}
	// repeat in the next call ...
    SetSCB(SCB_RETRYCB);
    SetNextFunSCB(web_hexdump);
    return;
}
/******************************************************************************
 * FunctionName : web saved flash
 * Description  : Processing the flash data send
 * Parameters   : none (Calback)
 * Returns      : none
*******************************************************************************/
void ICACHE_FLASH_ATTR web_get_flash(TCP_SERV_CONN *ts_conn)
{
    WEB_SRV_CONN *web_conn = (WEB_SRV_CONN *)ts_conn->linkd;
    // Check if this is a first round call
    if(CheckSCB(SCB_RETRYCB)==0) {
    	if(web_conn->udata_start == web_conn->udata_stop) return;
#if DEBUGSOO > 2
		os_printf("file_size:%08x ", web_conn->udata_stop - web_conn->udata_start );
#endif
    }
    // Get/put as many bytes as possible
    unsigned int len = mMIN(web_conn->msgbufsize - web_conn->msgbuflen, web_conn->udata_stop - web_conn->udata_start);
    // Read Flash addr = web_conn->webfinc_offsets, len = x, buf = sendbuf
#if DEBUGSOO > 2
	os_printf("%08x..%08x ",web_conn->udata_start, web_conn->udata_start + len );
#endif
    if(spi_flash_read(web_conn->udata_start, web_conn->msgbuf, len) == SPI_FLASH_RESULT_OK) {
      web_conn->udata_start += len;
      web_conn->msgbuflen += len;
      if(web_conn->udata_start < web_conn->udata_stop) {
        SetSCB(SCB_RETRYCB);
        SetNextFunSCB(web_get_flash);
        return;
      };
    };
    ClrSCB(SCB_RETRYCB);
//    SetSCB(SCB_FCLOSE | SCB_DISCONNECT);
    return;
}
/******************************************************************************
 * FunctionName : web saved flash
 * Description  : Processing the flash data send
 * Parameters   : none (Calback)
 * Returns      : none
*******************************************************************************/
void ICACHE_FLASH_ATTR web_get_ram(TCP_SERV_CONN *ts_conn)
{
    WEB_SRV_CONN *web_conn = (WEB_SRV_CONN *)ts_conn->linkd;
    // Check if this is a first round call
    if(CheckSCB(SCB_RETRYCB)==0) { // On initial call, проверка параметров
		if(web_conn->udata_start == web_conn->udata_stop) {
//    	    SetSCB(SCB_FCLOSE | SCB_DISCONNECT);
    	    return;
		}
#if DEBUGSOO > 2
		os_printf("file_size:%08x ", web_conn->udata_stop - web_conn->udata_start );
#endif
	}
    // Get/put as many bytes as possible
    uint32 len = mMIN(web_conn->msgbufsize - web_conn->msgbuflen, web_conn->udata_stop - web_conn->udata_start);
	copy_align4(web_conn->msgbuf, (void *)(web_conn->udata_start), len);
	web_conn->msgbuflen += len;
	web_conn->udata_start += len;
#if DEBUGSOO > 2
	os_printf("%08x-%08x ",web_conn->udata_start, web_conn->udata_start + len );
#endif
    if(web_conn->udata_start != web_conn->udata_stop) {
        SetSCB(SCB_RETRYCB);
        SetNextFunSCB(web_get_ram);
        return;
    };
    ClrSCB(SCB_RETRYCB);
//    SetSCB(SCB_FCLOSE | SCB_DISCONNECT);
    return;
}
/******************************************************************************
 * FunctionName : get_new_url
 * Parameters   : Parameters   : struct TCP_SERV_CONN
 * Returns      : string url
 ******************************************************************************/
void ICACHE_FLASH_ATTR get_new_url(TCP_SERV_CONN *ts_conn)
{
	WEB_SRV_CONN *web_conn = (WEB_SRV_CONN *)ts_conn->linkd;
	uint32 ip = 0;
	uint32 ip_ap = 0;
	uint32 ip_st = 0;
//	int z  = NULL_MODE;
	struct ip_info wifi_info;
	uint8 opmode = wifi_get_opmode();
	if(opmode == STATIONAP_MODE) {
		if((opmode & STATION_MODE) && wifi_get_ip_info(STATION_IF, &wifi_info)) {
			ip_st = wifi_info.ip.addr;
		}
		if((opmode & SOFTAP_MODE) && wifi_get_ip_info(SOFTAP_IF, &wifi_info)) {
			ip_ap = wifi_info.ip.addr;
		}
		if(((ts_conn->pcb->local_ip.addr^ip_ap) & 0xFFFFFF) == 0 || (ip_current_netif() != NULL && ip_current_netif()->ip_addr.addr == ip_ap)) {
			ip = ip_ap;
			opmode = SOFTAP_MODE;
		}
		else if(((ts_conn->pcb->local_ip.addr^ip_st) & 0xFFFFFF) == 0 || (ip_current_netif() != NULL && ip_current_netif()->ip_addr.addr == ip_st)) {
			ip = ip_st;
			opmode = STATION_MODE;
		};
	};
	opmode &= wificonfig.b.mode;
	if(opmode == 0) opmode = wificonfig.b.mode;
	if(wificonfig.ap.ipinfo.ip.addr == 0 || (opmode & SOFTAP_MODE)) {
		ip = wificonfig.ap.ipinfo.ip.addr;
		netbios_name[0]='A';
	} else {
		ip = wificonfig.st.ipinfo.ip.addr;
		netbios_name[0]='S';
	}
	if(syscfg.cfg.b.netbios_ena) tcp_strcpy(netbios_name);
	else {
		if(ip == 0) tcp_strcpy_fd("esp8266.ru"); // 0x0104A8C0
		else tcp_puts(IPSTR, IP2STR(&ip));
	};
	if(syscfg.web_port != 80) tcp_puts(":%u", syscfg.web_port);
}

/******************************************************************************
 * FunctionName : web_callback
 * Description  : callback
 * Parameters   : struct TCP_SERV_CONN
 * Returns      : none
 ******************************************************************************/
//#define ifcmp(a)   if(!os_memcmp((void*)cstr, a , sizeof(a)))
#define ifcmp(a)  if(rom_xstrcmp(cstr, a))

void ICACHE_FLASH_ATTR web_int_callback(TCP_SERV_CONN *ts_conn)
{
    WEB_SRV_CONN *web_conn = (WEB_SRV_CONN *)ts_conn->linkd;
        uint8 *cstr = &web_conn->msgbuf[web_conn->msgbuflen];
        {
           uint8 *vstr = os_strchr(cstr, '=');
           if(vstr != NULL) {
        	   *vstr++ = '\0';
        	   web_int_vars(ts_conn, cstr, vstr);
        	   return;
           }
        }
#if DEBUGSOO > 2
        os_printf("[%s]\n", cstr);
#endif
//        ifcmp("start") tcp_puts("0x%08x", web_conn->udata_start);
        ifcmp("start") tcp_puts("0x%08x", web_conn->udata_start);
        else ifcmp("stop") tcp_puts("0x%08x", web_conn->udata_stop);
        else ifcmp("xml_") {
            cstr+=4;
            web_conn->udata_start&=~3;
            ifcmp("ram") tcp_puts("0x%08x", *((uint32*)web_conn->udata_start));
            else tcp_put('?');
            web_conn->udata_start += 4;
        }
        else ifcmp("sys_") {
          cstr+=4;
          ifcmp("cid") tcp_puts("%08x", system_get_chip_id());
          else ifcmp("fid") tcp_puts("%08x", spi_flash_get_id());
          else ifcmp("fsize") tcp_puts("%u", spi_flash_real_size()); // flashchip->chip_size
          else ifcmp("sdkver") tcp_puts(system_get_sdk_version());
          else ifcmp("sysver") tcp_strcpy_fd(SYS_VERSION);
          else ifcmp("webver") tcp_strcpy_fd(WEB_SVERSION);
          else ifcmp("heap") tcp_puts("%u", system_get_free_heap_size());
          else ifcmp("adc") {
        	  uint16 x[4];
        	  read_adcs(x, 4);
        	  tcp_puts("%u", x[0]+x[1]+x[2]+x[3]); // 16 бит ADC :)
//        	  tcp_puts("%u", system_adc_read());
          }
          else ifcmp("time") tcp_puts("%u", system_get_time());
          else ifcmp("rtctime") tcp_puts("%u", system_get_rtc_time());
          else ifcmp("vdd33") tcp_puts("%u", readvdd33()); // system_get_vdd33() phy_get_vdd33();
          else ifcmp("wdt") tcp_puts("%u", ets_wdt_get_mode());
          else ifcmp("res_event") tcp_puts("%u", rtc_get_reset_reason()); // 1 - power/ch_pd, 2 - reset, 3 - software, 4 - wdt ...
          else ifcmp("rst") tcp_puts("%u", RTC_RAM_BASE[24] & 0xffff); // *((uint32 *)0x60001060 bit0..15: старт был по =1 reset, =0 ch_pd, bit16..31: deep_sleep_option
          else ifcmp("clkcpu") tcp_puts("%u", ets_get_cpu_frequency());
          else ifcmp("sleep_old") tcp_puts("%u",(*((uint32 *)0x60001060))>>16 ); // если нет отдельного питания RTC и deep_sleep не устанавливался/применялся при текущем включения питания чипа, то значение неопределенно (там хлам).
//          else ifcmp("test") tcp_puts("%d", cal_rf_ana_gain() ); //
#ifdef USE_ESPCONN
          else ifcmp("maxcns") tcp_puts("%d", espconn_tcp_get_max_con());
#endif
          else ifcmp("reset") web_conn->web_disc_cb = (web_func_disc_cb)_ResetVector;
          else ifcmp("restart") web_conn->web_disc_cb = (web_func_disc_cb)system_restart;
          else ifcmp("ram") tcp_puts("0x%08x", *((uint32 *)ahextoul(cstr+3)));
          else ifcmp("reg") tcp_puts("0x%08x", IOREG(ahextoul(cstr+3)));
          else ifcmp("ip") {
/*        	  uint32 cur_ip = 0;
        	  if(netif_default != NULL) cur_ip = netif_default->ip_addr.addr; */
        	  uint32 cur_ip = ts_conn->pcb->local_ip.addr;
//        	  if(cur_ip == 0) cur_ip = 0x0104A8C0;
			  tcp_puts(IPSTR, IP2STR(&cur_ip));
          }
          else ifcmp("url") get_new_url(ts_conn);
          else ifcmp("debug") tcp_puts("%u", system_get_os_print()&1); // system_set_os_print
          else ifcmp("const_") {
        	  cstr += 6;
        	  ifcmp("faddr") {
        		  tcp_puts("0x%08x", esp_init_data_default_addr);
        	  }
        	  else tcp_puts("%u", read_sys_const(ahextoul(cstr)));
          }
          else ifcmp("ucnst_") tcp_puts("%u", read_user_const(ahextoul(cstr+6)));
          else ifcmp("vdd") tcp_puts("%u", system_get_vdd33()); //  phy_get_vdd33();
          else ifcmp("netbios") {
       		  if(syscfg.cfg.b.netbios_ena) tcp_strcpy(netbios_name);
          }
          else tcp_put('?');
        }
        else ifcmp("cfg_") {
			cstr += 4;
			ifcmp("web_") {
				cstr += 4;
				ifcmp("port") tcp_puts("%u", syscfg.web_port);
				else ifcmp("twd") tcp_put((syscfg.cfg.b.web_time_wait_delete)? '1' : '0');
				else tcp_put('?');
			}
		    else ifcmp("tcp_") {
		    	cstr+=4;
		    	ifcmp("port") tcp_puts("%u", syscfg.tcp2uart_port);
	        	else ifcmp("twrec") tcp_puts("%u", syscfg.tcp2uart_twrec);
	        	else ifcmp("twcls") tcp_puts("%u", syscfg.tcp2uart_twcls);
	        	else ifcmp("url") {
	        		if(tcp2uart_url == NULL) tcp_puts("none");
	        		else tcp_puts("%s", tcp2uart_url);
	        	}
		    	else tcp_put('?');
		    }
		    else ifcmp("udp_") {
		    	cstr+=4;
		    	ifcmp("port") tcp_puts("%u", syscfg.udp_port);
		    	else tcp_put('?');
		    }
			else ifcmp("overclk") tcp_put((syscfg.cfg.b.hi_speed_enable)? '1' : '0');
			else ifcmp("pinclr") tcp_put((syscfg.cfg.b.pin_clear_cfg_enable)? '1' : '0');
			else ifcmp("debug") tcp_put((syscfg.cfg.b.debug_print_enable)? '1' : '0');
#ifdef USE_NETBIOS
			else ifcmp("netbios") tcp_put((syscfg.cfg.b.netbios_ena)? '1' : '0');
#endif
#ifdef USE_SNTP
			else ifcmp("sntp") tcp_put((syscfg.cfg.b.sntp_ena)? '1' : '0');
#endif
		    else tcp_put('?');
		}
        else ifcmp("wifi_") {
          cstr+=5;
          ifcmp("rdcfg") Read_WiFi_config(&wificonfig, WIFI_MASK_ALL);
          else ifcmp("newcfg") {
//        	  tcp_puts("%d", New_WiFi_config(WIFI_MASK_ALL));
        	  web_conn->web_disc_cb = (web_func_disc_cb)New_WiFi_config;
        	  web_conn->web_disc_par = WIFI_MASK_ALL;
          }
          else ifcmp("csta") tcp_puts("%d", wifi_station_get_connect_status());
          else ifcmp("mode") tcp_puts("%d", wifi_get_opmode());
          else ifcmp("id") tcp_puts("%d", wifi_station_get_current_ap_id());
          else ifcmp("phy") tcp_puts("%d", wifi_get_phy_mode());
          else ifcmp("chl") tcp_puts("%d", wifi_get_channel());
          else ifcmp("sleep") tcp_puts("%d", wifi_get_sleep_type());
          else ifcmp("scan") web_wscan_xml(ts_conn);
          else ifcmp("aucn") tcp_puts("%d", wifi_station_get_auto_connect());
          else ifcmp("power") { rom_en_pwdet(1); tcp_puts("%d", rom_get_power_db()); }
          else ifcmp("noise") tcp_puts("%d", get_noisefloor_sat()); // noise_init(), rom_get_noisefloor(), ram_get_noisefloor(), ram_set_noise_floor(),noise_check_loop(), read_hw_noisefloor(), ram_start_noisefloor()
          else ifcmp("hwnoise") tcp_puts("%d", read_hw_noisefloor());
          else ifcmp("fmsar") { // Test!
        	  rom_en_pwdet(1); // ?
        	  tcp_puts("%d", ram_get_fm_sar_dout(ahextoul(cstr+5)));
          }
          else ifcmp("rfopt") tcp_puts("%u",(RTC_RAM_BASE[24]>>16)&7); // system_phy_set_rfoption | phy_afterwake_set_rfoption(option); 0..4
          else ifcmp("vddpw") tcp_puts("%u", phy_in_vdd33_offset); // system_phy_set_tpw_via_vdd33(val); // = pphy_vdd33_set_tpw(vdd_x_1000); Adjust RF TX Power according to VDD33, unit: 1/1024V, range [1900, 3300]
          else ifcmp("maxpw") tcp_puts("%u", read_sys_const(34));// system_phy_set_max_tpw(val); // = phy_set_most_tpw(pow_db); unit: 0.25dBm, range [0, 82], 34th byte esp_init_data_default.bin
          else {
            uint8 if_index;
            ifcmp("ap_") if_index = SOFTAP_IF;
            else ifcmp("st_") if_index = STATION_IF;
            else { tcp_put('?'); return; };
            cstr+=3;
            ifcmp("dncp") {
            	if (if_index == SOFTAP_IF) tcp_puts("%d", (dhcps_flag==0)? 0 : 1);
            	else tcp_puts("%d", (dhcpc_flag==0)? 0 : 1);
            	return;
            };
            uint8 macaddr[6];
			wifi_get_macaddr(if_index, macaddr);
            uint8 opmode = wifi_get_opmode();
			struct ip_info wifi_info;
            if(if_index == SOFTAP_IF) {
              if (((opmode & SOFTAP_MODE) == 0) || (!wifi_get_ip_info(SOFTAP_IF, &wifi_info))) {
            	  wifi_info = wificonfig.ap.ipinfo;
            	  dhcps_lease = wificonfig.ap.ipdhcp;
//            	  p_dhcps_lease->start_ip = htonl(wificonfig.ap.ipdhcp.start_ip);
//            	  p_dhcps_lease->end_ip = htonl(wificonfig.ap.ipdhcp.end_ip);
//            	  p_dhcps_lease->start_ip = wificonfig.ap.ipdhcp.start_ip;
//            	  p_dhcps_lease->end_ip = wificonfig.ap.ipdhcp.end_ip;
              }
          	  ifcmp("sip") tcp_puts(IPSTR, IP2STR((struct ip_addr *)&dhcps_lease.start_ip));
          	  else ifcmp("eip") tcp_puts(IPSTR, IP2STR((struct ip_addr *)&dhcps_lease.end_ip));
          	  else ifcmp("ip") tcp_puts(IPSTR, IP2STR(&wifi_info.ip));
              else ifcmp("gw") tcp_puts(IPSTR, IP2STR(&wifi_info.gw));
              else ifcmp("mac") tcp_puts(MACSTR, MAC2STR(macaddr));
              else ifcmp("msk") tcp_puts(IPSTR, IP2STR(&wifi_info.netmask));
              else {
                  struct softap_config wicfgap;
                  if(((opmode & SOFTAP_MODE) == 0) || (!wifi_softap_get_config(&wicfgap)) ) wicfgap = wificonfig.ap.config;
                  ifcmp("ssid") tcp_htmlstrcpy(wicfgap.ssid, sizeof(wicfgap.ssid));
                  else ifcmp("psw") tcp_htmlstrcpy(wicfgap.password, sizeof(wicfgap.password));
                  else ifcmp("chl") tcp_puts("%d", wicfgap.channel);
                  else ifcmp("aum") tcp_puts("%d", wicfgap.authmode);
                  else ifcmp("hssid") tcp_puts("%d", wicfgap.ssid_hidden);
                  else ifcmp("mcns") tcp_puts("%d", wicfgap.max_connection);
                  else ifcmp("bint") tcp_puts("%d", wicfgap.beacon_interval);
                  else tcp_put('?');
              };
            }
            else {
            	ifcmp("rssi") tcp_puts("%d", wifi_station_get_rssi());
            	else {
                	if (((opmode & STATION_MODE) == 0) || (!wifi_get_ip_info(STATION_IF, &wifi_info))) wifi_info = wificonfig.st.ipinfo;
                    ifcmp("ip") tcp_puts(IPSTR, IP2STR(&wifi_info.ip));
                    else ifcmp("gw") tcp_puts(IPSTR, IP2STR(&wifi_info.gw));
                    else ifcmp("mac") tcp_puts(MACSTR, MAC2STR(macaddr));
                    else ifcmp("msk") tcp_puts(IPSTR, IP2STR(&wifi_info.netmask));
                    else {
                        struct station_config wicfgs;
                        if(((opmode & STATION_MODE) == 0) || (!wifi_station_get_config(&wicfgs)) ) wicfgs = wificonfig.st.config;
                        ifcmp("ssid") tcp_htmlstrcpy(wicfgs.ssid, sizeof(wicfgs.ssid));
                        else ifcmp("psw") tcp_htmlstrcpy(wicfgs.password, sizeof(wicfgs.password));
                        else ifcmp("sbss") tcp_puts("%d", wicfgs.bssid_set);
                        else ifcmp("bssid") tcp_puts(MACSTR, MAC2STR(wicfgs.bssid));
                        else tcp_put('?');
                    };
            	};
            };
          };
        }
        else ifcmp("uart_") {
            cstr+=5;
            volatile uint32 * uart_reg = uart0_;
            if(cstr[1] != '_' || cstr[0]<'0' || cstr[0]>'1' ) {
            	tcp_put('?');
            	return;
            }
            if(cstr[0] != '0') uart_reg = uart1_;
            cstr += 2;
            ifcmp("baud") tcp_puts("%u", (uint32) UART_CLK_FREQ / (uart_reg[IDX_UART_CLKDIV] & UART_CLKDIV_CNT));
            else ifcmp("reg")	tcp_puts("%08x", uart_reg[hextoul(cstr + 3)]);
            else ifcmp("parity") tcp_put((uart_reg[IDX_UART_CONF0] & UART_PARITY_EN)? '1':'0');
            else ifcmp("even") tcp_put((uart_reg[IDX_UART_CONF0] & UART_PARITY)? '1' : '0');
            else ifcmp("bits") tcp_puts("%u", (uart_reg[IDX_UART_CONF0] >> UART_BIT_NUM_S) & UART_BIT_NUM);
            else ifcmp("stop") tcp_puts("%u",(uart_reg[IDX_UART_CONF0] >> UART_STOP_BIT_NUM_S) & UART_STOP_BIT_NUM);
            else ifcmp("loopback") tcp_put((uart_reg[IDX_UART_CONF0] & UART_LOOPBACK)? '1' : '0');
            else ifcmp("flow") {
            	if(uart_reg == uart0_) tcp_put((uart0_flow_ctrl_flg)? '1' : '0');
            	else tcp_put((uart_reg[IDX_UART_CONF0]&UART_RX_FLOW_EN)? '1' : '0');
            }
            else ifcmp("rts_inv") tcp_put((uart_reg[IDX_UART_CONF0]&UART_RTS_INV)? '1' : '0');
            else ifcmp("cts_inv") tcp_put((uart_reg[IDX_UART_CONF0]&UART_CTS_INV)? '1' : '0');
            else ifcmp("rxd_inv") tcp_put((uart_reg[IDX_UART_CONF0]&UART_RXD_INV)? '1' : '0');
            else ifcmp("txd_inv") tcp_put((uart_reg[IDX_UART_CONF0]&UART_TXD_INV)? '1' : '0');
            else ifcmp("dsr_inv") tcp_put((uart_reg[IDX_UART_CONF0]&UART_DSR_INV)? '1' : '0');
            else ifcmp("dtr_inv") tcp_put((uart_reg[IDX_UART_CONF0]&UART_DTR_INV)? '1' : '0');
            else tcp_put('?');
        }
        else ifcmp("bin_") {
        	cstr+=4;
        	ifcmp("flash") {
        		cstr+=5;
        		if(*cstr == '_') {
        			cstr++;
            		ifcmp("all") {
            	    	web_conn->udata_start = 0;
            	    	web_conn->udata_stop = spi_flash_real_size();
            	    	web_get_flash(ts_conn);
            		}
            		else ifcmp("sec_") {
            			web_conn->udata_start = ahextoul(cstr+4) << 12;
            			web_conn->udata_stop = web_conn->udata_start + SPI_FLASH_SEC_SIZE;
            			web_get_flash(ts_conn);
            		}
            		else ifcmp("const") {
            	    	web_conn->udata_start = esp_init_data_default_addr;
            	    	web_conn->udata_stop = web_conn->udata_start + SIZE_USER_CONST;
            	    	web_get_flash(ts_conn);
            		}
            		else ifcmp("disk") {
            			web_conn->udata_start = WEBFS_base_addr();
            			web_conn->udata_stop = web_conn->udata_start + WEBFS_curent_size();
            			web_get_flash(ts_conn);
            		}
            		else tcp_put('?');
        		}
        		else web_get_flash(ts_conn);
        	}
        	else ifcmp("ram") web_get_ram(ts_conn);
        }
        else ifcmp("hexdmp") {
        	if(cstr[6]=='d') ts_conn->flag.user_option1 = 1;
        	else ts_conn->flag.user_option1 = 0;
        	web_hexdump(ts_conn);
        }
        else ifcmp("hellomsg") tcp_puts_fd("Web on ESP8266!");
        else ifcmp("tcp_") {
        	cstr+=4;
        	ifcmp("connected") {
        		if(tcp2uart_conn == NULL) tcp_put('0');
        		else tcp_put('1');
        	}
        	else ifcmp("remote") {
        		if(tcp2uart_conn!=NULL) {
        			tcp_puts(IPSTR ":%d", IP2STR(&(tcp2uart_conn->remote_ip.dw)), tcp2uart_conn->remote_port);
        		}
        		else tcp_strcpy_fd("closed");
        	}
        	else ifcmp("host") {
        		if(tcp2uart_conn!=NULL) tcp_puts(IPSTR ":%d", IP2STR(&(tcp2uart_conn->pcb->local_ip.addr)), tcp2uart_conn->pcb->local_port);
        		else tcp_strcpy_fd("none");
        	}
       		else ifcmp("twrec") {
       			if(tcp2uart_conn!=NULL) tcp_puts("%u", tcp2uart_conn->pcfg->time_wait_rec);
       			else tcp_strcpy_fd("closed");
       		}
       		else ifcmp("twcls") {
       			if(tcp2uart_conn!=NULL) tcp_puts("%u", tcp2uart_conn->pcfg->time_wait_cls);
       			else tcp_strcpy_fd("closed");
       		}
       		else tcp_put('?');
        }
        else ifcmp("web_") {
        	cstr+=4;
        	ifcmp("port") tcp_puts("%u", ts_conn->pcfg->port);
        	else ifcmp("host") tcp_puts(IPSTR ":%d", IP2STR(&(ts_conn->pcb->local_ip.addr)), ts_conn->pcb->local_port);
        	else ifcmp("remote") tcp_puts(IPSTR ":%d", IP2STR(&(ts_conn->remote_ip.dw)), ts_conn->remote_port);
        	else ifcmp("twrec") tcp_puts("%u", ts_conn->pcfg->time_wait_rec);
        	else ifcmp("twcls") tcp_puts("%u", ts_conn->pcfg->time_wait_cls);
        	else tcp_put('?');
        }
        else ifcmp("wfs_") {
        	cstr+=4;
        	ifcmp("files") tcp_puts("%u", numFiles);
        	else ifcmp("addr") tcp_puts("0x%08x", WEBFS_base_addr());
        	else ifcmp("size") tcp_puts("%u", WEBFS_curent_size());
        	else ifcmp("max_size") tcp_puts("%u", WEBFS_max_size());
        	else tcp_put('?');
        }
        else ifcmp("test_adc") web_test_adc(ts_conn);
        else ifcmp("gpio") {
            cstr+=4;
        	if((*cstr>='0')&&(*cstr<='9')) {
        		uint32 n = atoi(cstr);
        		cstr++;
        		if(*cstr != '_') cstr++;
        		if(*cstr == '_' && n < 16) {
        			cstr++;
        			ifcmp("inp") {
        				if(GPIO_IN & (1<<n)) tcp_put('1');
        				else tcp_put('0');
        			}
        			else ifcmp("out") {
        				if(GPIO_OUT &(1<<n)) tcp_put('1');
        				else tcp_put('0');
        			}
        			else ifcmp("dir") {
        				if(GPIO_ENABLE & (1<<n)) tcp_put('1');
        				else tcp_put('0');
        			}
        			else ifcmp("fun") {
        				uint32 x = GET_PIN_FUNC(n);
        				x = (x & 3) | ((x >> 2) & 4);
        				tcp_puts("%u", x);
        			}
        			else ifcmp("pull") {
        				uint32 x = GPIOx_MUX(n) >> 6;
        				tcp_puts("%u", x & 3);
        			}
    	            else ifcmp("opd") tcp_put((GPIOx_PIN(n) & (1 << 2))? '1' : '0');
    	            else ifcmp("pu") tcp_put((GPIOx_MUX(n) & (1 << 7))? '1' : '0');
    	            else ifcmp("pd") tcp_put((GPIOx_MUX(n) & (1 << 6))? '1' : '0');

        			else tcp_put('?');
        		}
        		else tcp_put('?');
        	}
        	else if(*cstr == '_') {
        		cstr++;
        		ifcmp("inp") tcp_puts("%u", GPIO_IN & 0xFFFF);
        		else ifcmp("out") tcp_puts("%u", GPIO_OUT & 0xFFFF);
        		else ifcmp("dir") tcp_puts("%u", GPIO_ENABLE & 0xFFFF);
            	else tcp_put('?');
        	}
        	else tcp_put('?');
        }
#ifdef USE_SNTP
		else ifcmp("sntp_") {
			cstr += 5;
			ifcmp("time") tcp_puts("%u", get_sntp_time());
			else tcp_put('?');
		}
#endif
        else tcp_put('?');
}

