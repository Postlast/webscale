/******************************************************************************
 * FileName: webserver.c
 * Description: The web server mode configration.
*******************************************************************************/

#include "user_config.h"
#include "bios.h"
#include "add_sdk_func.h"
#include "hw/esp8266.h"
#include "hw/eagle_soc.h"
#include "hw/uart_register.h"
#include "os_type.h"
#include "osapi.h"
#include "user_interface.h"

#include "lwip/ip.h"
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"

#include "tcp_srv_conn.h"
#include "web_srv_int.h"
#include "web_utils.h"
#include "tcp2uart.h"
#include "wifi.h"
#include "flash_eep.h"
#include "driver/sigma_delta.h"
#include "sys_const.h"
#include "rom2ram.h"

#ifdef USE_NETBIOS
#include "netbios.h"
#endif

#ifdef USE_SNTP
#include "sntp.h"
#endif

#ifdef USE_LWIP_PING
#include "lwip/app/ping.h"
struct ping_option pingopt; // for test
#endif

extern TCP_SERV_CONN * tcp2uart_conn;

typedef uint32 (* call_func)(uint32 a, uint32 b, uint32 c);

void ICACHE_FLASH_ATTR reg_sct_bits(volatile uint32 * addr, uint32 bits, uint32 val)
{
	uint32 x = *addr;
	if(val == 3) x ^= bits;
	else if(val) x |= bits;
	else x &= ~ bits;
	*addr =  x;
}
/******************************************************************************
 * FunctionName : parse_url
 * Description  : parse the received data from the server
 * Parameters   : CurHTTP -- the result of parsing the url
 * Returns      : none
*******************************************************************************/
// #define ifcmp(a)  if(!os_memcmp((void*)cstr, a, sizeof(a)))
#define ifcmp(a)  if(rom_xstrcmp(cstr, a))

void ICACHE_FLASH_ATTR web_int_vars(TCP_SERV_CONN *ts_conn, uint8 *pcmd, uint8 *pvar)
{
    WEB_SRV_CONN *web_conn = (WEB_SRV_CONN *)ts_conn->linkd;
	uint32 val = ahextoul(pvar);
	char *cstr = pcmd;
#if DEBUGSOO > 2
    os_printf("[%s=%s]\n", pcmd, pvar);
#endif
	ifcmp("start") 		web_conn->udata_start = val;
	else ifcmp("stop") 	web_conn->udata_stop = val;
	else ifcmp("sys_") {
		cstr+=4;
		ifcmp("restart") {
			if(val == 12345) 	web_conn->web_disc_cb = (web_func_disc_cb)system_restart;
		}
		else ifcmp("reset") {
			if(val == 12345) web_conn->web_disc_cb = (web_func_disc_cb)_ResetVector;
		}
#ifdef USE_ESPCONN
		else ifcmp("maxcns") 	espconn_tcp_set_max_con(((val<=5)&&(val>0))? val : 5);
#endif
		else ifcmp("ram") 	{ uint32 ptr = ahextoul(cstr+3)&0xfffffffc; *((uint32 *)ptr) = val; }
		else ifcmp("debug") 	system_set_os_print(val);
#ifdef USE_LWIP_PING
		else ifcmp("ping") {
//			struct ping_option *pingopt = (struct ping_option *)UartDev.rcv_buff.pRcvMsgBuff;
			pingopt.ip = ipaddr_addr(pvar);
			pingopt.count = 3;
			pingopt.recv_function=NULL;
			pingopt.sent_function=NULL;
			ping_start(&pingopt);
		}
#endif
		else ifcmp("sleep_") {
			cstr += 6;
			ifcmp("option") 	system_deep_sleep_set_option(val);
			else ifcmp("us") {
				web_conn->web_disc_cb = (web_func_disc_cb)system_deep_sleep;
				web_conn->web_disc_par = val;
			}
#if DEBUGSOO > 5
			else os_printf(" - none!\n");
#endif
		}
		else ifcmp("const_")	write_sys_const(ahextoul(cstr+6), val);
		else ifcmp("ucnst_") 	write_user_const(ahextoul(cstr+6), val);
		else ifcmp("clkcpu") {
			if(val > 80) {
				CLK_PRE_PORT |= 1;
				os_update_cpu_frequency(160);
			}
			else
			{
				CLK_PRE_PORT &= ~1;
				os_update_cpu_frequency(80);
			}
		}
#if DEBUGSOO > 5
		else os_printf(" - none!\n");
#endif
    }
	else ifcmp("cfg_") {
		cstr += 4;
		ifcmp("web_") {
			cstr += 4;
			ifcmp("port") {
				if(syscfg.web_port != val) {
	        		web_conn->web_disc_par = syscfg.web_port; // ts_conn->pcfg->port
					syscfg.web_port = val;
	    			web_conn->web_disc_cb = (web_func_disc_cb)webserver_reinit;
				}
			}
			else ifcmp("twd") {
				if(val) {
					syscfg.cfg.b.web_time_wait_delete = 1;
					ts_conn->pcfg->flag.pcb_time_wait_free = 1;
				}
				else {
					syscfg.cfg.b.web_time_wait_delete = 0;
					ts_conn->pcfg->flag.pcb_time_wait_free = 0;
				}
			}
#if DEBUGSOO > 5
			else os_printf(" - none!\n");
#endif
		}
	    else ifcmp("tcp_") {
	    	cstr+=4;
	    	ifcmp("port") tcp2uart_start(val);
	   		else ifcmp("twrec") {
	   			syscfg.tcp2uart_twrec = val;
	   			if(tcp2uart_servcfg != NULL) {
	   				tcp2uart_servcfg->time_wait_rec = val;
	   			}
	   		}
	   		else ifcmp("twcls") {
	   			syscfg.tcp2uart_twcls = val;
	   			if(tcp2uart_servcfg != NULL) {
	   				tcp2uart_servcfg->time_wait_cls = val;
	   			}
	   		}
			else ifcmp("url") {
				if(new_tcp2uart_url(pvar))
					tcp2uart_start(syscfg.tcp2uart_port);
			}
	    }
	    else ifcmp("udp_") {
	    	cstr+=4;
	    	ifcmp("port") 	syscfg.udp_port = val;
	    }
		else ifcmp("overclk") 	syscfg.cfg.b.hi_speed_enable = (val)? 1 : 0;
		else ifcmp("pinclr") 	syscfg.cfg.b.pin_clear_cfg_enable = (val)? 1 : 0;
		else ifcmp("debug") {
			val &= 1;
			syscfg.cfg.b.debug_print_enable = val;
			system_set_os_print(val);
			update_mux_txd1();
		}
		else ifcmp("save") {
			if(val == 2) SetSCB(SCB_SYSSAVE); // по закрытию соединения вызвать sys_write_cfg()
			else if(val == 1) sys_write_cfg();
		}
#ifdef USE_NETBIOS
		else ifcmp("netbios") {
			syscfg.cfg.b.netbios_ena = (val)? 1 : 0;
			if(syscfg.cfg.b.netbios_ena) netbios_init();
			else netbios_off();
		}
#endif
#ifdef USE_SNTP
		else ifcmp("sntp") {
			syscfg.cfg.b.sntp_ena = (val)? 1 : 0;
			if(syscfg.cfg.b.sntp_ena) sntp_init();
			else sntp_close();
		}
#endif
#if DEBUGSOO > 5
		else os_printf(" - none!\n");
#endif
		// sys_write_cfg();
	}
    else ifcmp("wifi_") {
      cstr+=5;
      ifcmp("rdcfg") web_conn->udata_stop = Read_WiFi_config(&wificonfig, val);
      else ifcmp("newcfg") {
    	  web_conn->web_disc_cb = (web_func_disc_cb)New_WiFi_config;
    	  web_conn->web_disc_par = val;
/*    	  web_conn->udata_stop = New_WiFi_config(val);
#if DEBUGSOO > 2
          os_printf("New_WiFi_config(0x%p) = 0x%p ", val, web_conn->udata_stop);
#endif */
      }
      else ifcmp("mode")	wificonfig.b.mode = ((val&3)==0)? 3 : val;
      else ifcmp("phy")  	wificonfig.b.phy = val;
      else ifcmp("chl") 	wificonfig.b.chl = val;
      else ifcmp("sleep") 	wificonfig.b.sleep = val;
      else ifcmp("scan") {
//    	  web_conn->web_disc_par = val;
    	  web_conn->web_disc_cb = (web_func_disc_cb)wifi_start_scan;
      }
      else ifcmp("save") { wifi_save_fcfg(val); }
      else ifcmp("read") {
    	  wifi_read_fcfg();
    	  if(val) {
    		  web_conn->udata_stop = Set_WiFi(&wificonfig, val);
#if DEBUGSOO > 2
    		  os_printf("WiFi set:%p\n", web_conn->udata_stop);
#endif
    	  }
    	  else web_conn->udata_stop = 0;
      }
      else ifcmp("rfopt") system_phy_set_rfoption(val); // phy_afterwake_set_rfoption(val); // phy_afterwake_set_rfoption(option);
      else ifcmp("vddpw") system_phy_set_tpw_via_vdd33(val); // = pphy_vdd33_set_tpw(vdd_x_1000); Adjust RF TX Power according to VDD33, unit: 1/1024V, range [1900, 3300]
      else ifcmp("maxpw") system_phy_set_max_tpw(val); // = phy_set_most_tpw(pow_db); unit: 0.25dBm, range [0, 82], 34th byte esp_init_data_default.bin
      else ifcmp("ap_") {
    	  cstr+=3;
          ifcmp("ssid") {
        	  if(pvar[0]!='\0'){
        		  os_memset(wificonfig.ap.config.ssid, 0, sizeof(wificonfig.ap.config.ssid));
        		  int len = os_strlen(pvar);
        		  if(len > sizeof(wificonfig.ap.config.ssid)) {
        			  len = sizeof(wificonfig.ap.config.ssid);
        		  }
        		  os_memcpy(wificonfig.ap.config.ssid, pvar, len);
        		  wificonfig.ap.config.ssid_len = len;
        	  }
          }
          else ifcmp("psw") {
    		  int len = os_strlen(pvar);
    		  if(len > (sizeof(wificonfig.ap.config.password) - 1)) {
    			  len = sizeof(wificonfig.ap.config.password) - 1;
    		  }
    		  os_memset(&wificonfig.ap.config.password, 0, sizeof(wificonfig.ap.config.password));
    		  os_memcpy(wificonfig.ap.config.password, pvar, len);
          }
          else ifcmp("dncp")	wificonfig.b.ap_dhcp_enable = val;
          else ifcmp("chl") 	wificonfig.ap.config.channel = val;
          else ifcmp("aum") 	wificonfig.ap.config.authmode = val;
          else ifcmp("hssid") 	wificonfig.ap.config.ssid_hidden = val;
          else ifcmp("mcns") 	wificonfig.ap.config.max_connection = val;
    	  else ifcmp("bint") 	wificonfig.ap.config.beacon_interval = val;
    	  else ifcmp("ip") 		wificonfig.ap.ipinfo.ip.addr = ipaddr_addr(pvar);
          else ifcmp("gw") 		wificonfig.ap.ipinfo.gw.addr = ipaddr_addr(pvar);
          else ifcmp("msk") 	wificonfig.ap.ipinfo.netmask.addr = ipaddr_addr(pvar);
          else ifcmp("mac") 	strtomac(pvar,wificonfig.ap.macaddr);
    	  else ifcmp("sip") 	wificonfig.ap.ipdhcp.start_ip.addr = ipaddr_addr(pvar);
    	  else ifcmp("eip") 	wificonfig.ap.ipdhcp.end_ip.addr = ipaddr_addr(pvar);
#if DEBUGSOO > 2
          else os_printf(" - none! ");
#endif
      }
      else ifcmp("st_") {
    	  cstr+=3;
          ifcmp("dncp") 		wificonfig.b.st_dhcp_enable = val;
          else ifcmp("aucn") 	wificonfig.st.auto_connect = val;
          ifcmp("ssid") {
        	  if(pvar[0]!='\0'){
        		  os_memset(wificonfig.st.config.ssid, 0, sizeof(wificonfig.st.config.ssid));
        		  int len = os_strlen(pvar);
        		  if(len > sizeof(wificonfig.st.config.ssid)) {
        			  len = sizeof(wificonfig.st.config.ssid);
        		  }
        		  os_memcpy(wificonfig.st.config.ssid, pvar, len);
        	  }
          }
          else ifcmp("psw") {
    		  int len = os_strlen(pvar);
    		  if(len > (sizeof(wificonfig.st.config.password) - 1)) {
    			  len = sizeof(wificonfig.st.config.password) - 1;
    		  }
    		  os_memset(&wificonfig.st.config.password, 0, sizeof(wificonfig.st.config.password));
    		  os_memcpy(wificonfig.st.config.password, pvar, len);
          }
          else ifcmp("sbss") 	wificonfig.st.config.bssid_set = val;
          else ifcmp("bssid") 	strtomac(pvar, wificonfig.st.config.bssid);
    	  else ifcmp("ip") 		wificonfig.st.ipinfo.ip.addr = ipaddr_addr(pvar);
          else ifcmp("gw") 		wificonfig.st.ipinfo.gw.addr = ipaddr_addr(pvar);
          else ifcmp("msk") 	wificonfig.st.ipinfo.netmask.addr = ipaddr_addr(pvar);
          else ifcmp("mac") 	strtomac(pvar,wificonfig.st.macaddr);
#if DEBUGSOO > 5
          else os_printf(" - none!\n");
#endif
      }
#if DEBUGSOO > 5
      else os_printf(" - none!\n");
#endif
    }
    else ifcmp("uart_") {
        cstr+=5;
        ifcmp("save") uart_save_fcfg(val);
        else ifcmp("read") uart_read_fcfg(val);
        else {
            int n = 0;
            if(cstr[1] != '_' || cstr[0]<'0' || cstr[0]>'1' ) {
#if DEBUGSOO > 5
            	os_printf(" - none! ");
#endif
            }
            if(cstr[0] != '0') n++;
            cstr += 2;
            ifcmp("baud") {
//                UartDev.baut_rate = val;
                uart_div_modify(n, UART_CLK_FREQ / val);
            }
            else ifcmp("parity") 	WRITE_PERI_REG(UART_CONF0(n), (READ_PERI_REG(UART_CONF0(n)) & (~UART_PARITY_EN)) | ((val)? UART_PARITY_EN : 0));
            else ifcmp("even") 	 	WRITE_PERI_REG(UART_CONF0(n), (READ_PERI_REG(UART_CONF0(n)) & (~UART_PARITY)) | ((val)? UART_PARITY : 0));
            else ifcmp("bits") 		WRITE_PERI_REG(UART_CONF0(n), (READ_PERI_REG(UART_CONF0(n)) & (~(UART_BIT_NUM << UART_BIT_NUM_S))) | ((val & UART_BIT_NUM)<<UART_BIT_NUM_S));
            else ifcmp("stop") 		WRITE_PERI_REG(UART_CONF0(n), (READ_PERI_REG(UART_CONF0(n)) & (~(UART_STOP_BIT_NUM << UART_STOP_BIT_NUM_S))) | ((val & UART_STOP_BIT_NUM)<<UART_STOP_BIT_NUM_S));
            else ifcmp("loopback") 	WRITE_PERI_REG(UART_CONF0(n), (READ_PERI_REG(UART_CONF0(n)) & (~UART_LOOPBACK)) | ((val)? UART_LOOPBACK : 0));
            else ifcmp("flow") {
            	if(n==0) uart0_set_flow((val!=0));
            }
            else ifcmp("rts_inv") set_uartx_invx(n, val, UART_RTS_INV);
            else ifcmp("dtr_inv") set_uartx_invx(n, val, UART_DTR_INV);
            else ifcmp("cts_inv") set_uartx_invx(n, val, UART_CTS_INV);
            else ifcmp("rxd_inv") set_uartx_invx(n, val, UART_RXD_INV);
            else ifcmp("txd_inv") set_uartx_invx(n, val, UART_TXD_INV);
            else ifcmp("dsr_inv") set_uartx_invx(n, val, UART_DSR_INV) ;
#if DEBUGSOO > 5
            else os_printf(" - none! ");
#endif
        }
#if DEBUGSOO > 5
        else os_printf(" - none! ");
#endif
    }
    else ifcmp("gpio") {
        cstr+=4;
    	if((*cstr>='0')&&(*cstr<='9')) {
    		uint32 n = atoi(cstr);
    		cstr++;
    		if(*cstr != '_') cstr++;
    		if(*cstr == '_' && n < 16) {
    			cstr++;
	    		ifcmp("set") { if(val) GPIO_OUT_W1TS = 1 << n; }
	            else ifcmp("clr") { if(val) GPIO_OUT_W1TC = 1 << n; }
	            else ifcmp("out") {
	            	if(val == 3) {
	            		if(GPIO_OUT &(1<<n)) GPIO_OUT_W1TC = 1 << n;
	            		else GPIO_OUT_W1TS = 1 << n;
	            	}
	            	else if(val == 1) GPIO_OUT_W1TS = 1 << n;
	            	else GPIO_OUT_W1TC = 1 << n;
	            }
	            else ifcmp("ena") { if(val) GPIO_ENABLE_W1TS = 1 << n; }
	            else ifcmp("dis") { if(val) GPIO_ENABLE_W1TC = 1 << n; }
	            else ifcmp("dir") {
	            	if(val == 3) {
	            		if(GPIO_ENABLE & (1<<n)) GPIO_ENABLE_W1TC = 1 << n;
	            		else GPIO_ENABLE_W1TS = 1 << n;
	            	}
	            	else if(val == 1) GPIO_ENABLE_W1TS = 1 << n;
	            	else GPIO_ENABLE_W1TC =  1 << n;
	            }
	            else ifcmp("fun")	{ SET_PIN_FUNC(n,val); }
	            else ifcmp("io") 	{ SET_PIN_FUNC_IOPORT(n); }
	            else ifcmp("def") 	{ SET_PIN_FUNC_DEF_SDK(n); }
	            else ifcmp("sgs") 	{ sigma_delta_setup(n); set_sigma_duty_312KHz(val); }
	            else ifcmp("sgc") 	sigma_delta_close(n);
	            else ifcmp("sgn") 	set_sigma_duty_312KHz(val);
	            else ifcmp("od") 	reg_sct_bits(&GPIOx_PIN(n), BIT2, val);
	            else ifcmp("pu") 	reg_sct_bits(&GPIOx_MUX(n), BIT7, val);
	            else ifcmp("pd") 	reg_sct_bits(&GPIOx_MUX(n), BIT6, val);
    		}
    	}
    	else if(*cstr == '_') {
    		cstr++;
    		ifcmp("set") 		GPIO_OUT_W1TS = val;
            else ifcmp("clr") 	GPIO_OUT_W1TC = val;
            else ifcmp("out") 	GPIO_OUT = val;
            else ifcmp("ena") 	GPIO_ENABLE_W1TS = val;
            else ifcmp("dis") 	GPIO_ENABLE_W1TC = val;
            else ifcmp("dir") 	GPIO_ENABLE = val;
    	}
    }
    else ifcmp("hexdmp") {
    	if(web_conn->bffiles[0]==WEBFS_WEBCGI_HANDLE && CheckSCB(SCB_GET)) {
    		if(val > 0) {
    	    	if(cstr[6]=='d') ts_conn->flag.user_option1 = 1;
    	    	else ts_conn->flag.user_option1 = 0;
    			uint32 x = ahextoul(cstr+7);
    			if(x >= 0x20000000) {
    				web_conn->udata_start = x;
    			};
    			web_conn->udata_stop = val + web_conn->udata_start;
    			SetSCB(SCB_RETRYCB | SCB_FCALBACK);
    			SetNextFunSCB(web_hexdump);
    		};
    	}
    }
	else ifcmp("call") {
		call_func ptr = (call_func)(ahextoul(cstr+4)&0xfffffffc);
		web_conn->udata_stop = ptr(val, web_conn->udata_start, web_conn->udata_stop);
#if DEBUGSOO > 0
		os_printf("%p=call_func() ", web_conn->udata_stop);
#endif
	}
    else ifcmp("web_") {
    	cstr+=4;
    	ifcmp("port") {
    			web_conn->web_disc_cb = (web_func_disc_cb)webserver_init;
        		web_conn->web_disc_par = val;
    	}
    	else ifcmp("close") {
			web_conn->web_disc_cb = (web_func_disc_cb)webserver_close;
			web_conn->web_disc_par = val;
    	}
    	else ifcmp("twrec") ts_conn->pcfg->time_wait_rec = val;
    	else ifcmp("twcls") ts_conn->pcfg->time_wait_cls = val;
#if DEBUGSOO > 5
    	else os_printf(" - none! ");
#endif
    }
    else ifcmp("tcp_") {
    	cstr+=4;
   		ifcmp("twrec") {
   			if(tcp2uart_servcfg != NULL) {
   				tcp2uart_servcfg->time_wait_rec = val;
   			}
   		}
   		else ifcmp("twcls") {
   			if(tcp2uart_servcfg != NULL) {
   			   	tcp2uart_servcfg->time_wait_cls = val;
   			}
   		}
    }
	else ifcmp("test") {
	}
#if DEBUGSOO > 5
    else os_printf(" - none! ");
#endif
}

