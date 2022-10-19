#define USE_OTA

#include <Arduino.h>

#include <painlessMesh.h>

#ifdef USE_OTA
#include <ESP32httpUpdate.h>

#define PROG        "firmware/espressif32/MONITOR_MESH/main/firmware"
#endif

#define MESH_SSID     "pharmdata"
#define MESH_PASSWORD "4026892842"
#define MESH_PORT     5555

#define RPTINV      1
#define RUNTIME     30
#define SLEEPINV    300

typedef struct
{
  const char * ssid;
  const char * passwd;
  const char * updateAddr;
} WiFiInfo;

WiFiInfo wifiInfo_g[] =
{
  { "s1616", "4026892842", "pharmdata.ddns.net" },
  { "lambhome", "4022890568", "pharmdata.ddns.net" },
  { "sheepshed-mifi", "4026892842", "pharmdata.ddns.net" },
  { "Jimmy-MiFi", "4026892842", "pharmdata.ddns.net" },
  { NULL }
};

static WiFiInfo * wifi_g = NULL;

#ifdef USE_OTA

static void _progress(
        size_t aDone,
        size_t aTotal
        )
    {
    static bool ledState = false;
    static unsigned int cntr = 0;

    cntr ++;
    if ( (cntr % 5) == 0 )
        {
        digitalWrite( LED_BUILTIN, ledState ? HIGH : LOW );
        ledState = !ledState;

        size_t pct10;

        pct10 = (aDone * 1000) /aTotal;

        Serial.printf( "%u/%u(%%%d.%d)\r", aDone, aTotal, pct10 / 10, pct10 % 10 );
        }
    }

void _checkUpdate(
        )
    {
    Serial.printf( "(%d) checkUpdate - (repo: %s) file: %s\r\n", 
            __LINE__, (wifi_g != NULL) ? wifi_g->updateAddr : "NULL", PROG );

    if ( wifi_g != NULL )
        {
        Update.onProgress( _progress );

        ESPhttpUpdate.rebootOnUpdate( true );

        t_httpUpdate_return ret = ESPhttpUpdate.update( wifi_g->updateAddr, 80,
                "/firmware/update.php", PROG );

/*
        if ( ESPhttpUpdate.getLastError() != 0 )
            {
*/
	        Serial.printf( "(%d) %s\r\n", 
                    ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str() );
/*
            }
*/

        switch( ret )
            {
            case HTTP_UPDATE_FAILED:
                // Serial.printf( "HTTP_UPDATE_FAILED(%d)\r\n", ret );
                break;

            case HTTP_UPDATE_NO_UPDATES:
                // Serial.printf( "HTTP_UPDATE_NO_UPDATES(%d)\r\n", ret );
                break;

            case HTTP_UPDATE_OK:
                // Serial.printf( "HTTP_UPDATE_OK(%d)\r\n", ret );
                break;
            }
        }
    }
#endif

/*
    virtual int init() { return esp_now_init(); }
    int reg_send_cb(esp_now_send_cb_t cb) {
        return esp_now_register_send_cb(cb);
    }
    int reg_recv_cb(esp_now_recv_cb_t cb) {
        return esp_now_register_recv_cb(cb);
    }
*/

static char mac[ 40 ];
static int8_t batLevel_g = 0;
static uint64_t stTime_g;

Scheduler userScheduler;

typedef std::function<void(String &from, String &msg)> namedReceivedCallback_t;

class MeshWrapper 
        : public painlessMesh
    {
  public:
    MeshWrapper(
            ) 
        {
        auto cb = [this](
                uint32_t aFromAddr, String & aMsg
                ) 
            {
            if ( aMsg == "GW" )
                {
                Serial.printf( "Received a GW from (0x%X)\n", aFromAddr );
/*
                painlessMesh::sendSingle( aFromAddr, String( "IM" ) );
                Serial.printf( "IM sent to (0x%X)\n", aFromAddr );
*/
                return;
                }

            Serial.printf( "%u(0x%X) - %s\n",
                     aFromAddr, aFromAddr, aMsg.c_str() );
            };

        painlessMesh::onReceive( cb );
        }

    String getName(
            ) 
        {
        return nodeName;
        }

    void setName(
            String &name
            ) 
        {
        nodeName = name;
        }

    using painlessMesh::sendSingle;

    bool sendSingle(
            String &aName, 
            String &aMsg
            ) 
        {
        // Look up name
        for (auto && pr : nameMap) 
            {
            if (aName.equals(pr.second)) 
                {
                uint32_t to = pr.first;
                return painlessMesh::sendSingle(to, aMsg);
                }
            }
        return false;
        }

    virtual void stop() 
        {
        painlessMesh::stop();
        }

    virtual void onReceive(
            painlessmesh::receivedCallback_t onReceive
            ) 
        {
        userReceivedCallback = onReceive;
        }

    void onReceive(
            namedReceivedCallback_t onReceive
            ) 
        {
        userNamedReceivedCallback = onReceive;
        }
    
  protected:
    String nodeName;
    std::map<uint32_t, String> nameMap;

    painlessmesh::receivedCallback_t userReceivedCallback;
    namedReceivedCallback_t          userNamedReceivedCallback;
    };

static MeshWrapper mesh;
static String nodeName_g;

static void _setupMesh(
        )
    {
    WiFi.disconnect( false, true );

    mesh.setDebugMsgTypes( ERROR );

    // mesh.setDebugMsgTypes( COMMUNICATION | CONNECTION | DEBUG | ERROR | MESH_STATUS | REMOTE );

    mesh.init( MESH_SSID, MESH_PASSWORD, &userScheduler, MESH_PORT,  WIFI_AP_STA, 6 );

    mesh.setName( nodeName_g );

    mesh.onNewConnection( [](
            uint32_t aNodeId
            )
        {
        SimpleList< uint32_t > nodes;
        nodes = mesh.getNodeList();

        Serial.printf( "\n(onNewConnection) Num Nodes: %d\n", nodes.size() );
        Serial.println( "Connection List:" );
        SimpleList<uint32_t>::iterator n = nodes.begin();
        while( n != nodes.end() )
            {
            Serial.printf( "%u(0x%X)", *n, *n );
            n ++;
            }
        Serial.println( "" );
        } );

    mesh.onDroppedConnection( [](
            uint32_t aNodeId
            )
        {
        SimpleList< uint32_t > nodes;
        nodes = mesh.getNodeList();

        Serial.printf( "\n(onDroppedConnection) (0x%X) Num Nodes: %d\n", aNodeId, nodes.size() );
        Serial.println( "Connection List:" );
        SimpleList<uint32_t>::iterator n = nodes.begin();
        while( n != nodes.end() )
            {
            Serial.printf( "(0x%X)", *n );
            n ++;
            }
        Serial.println( "" );
        } );

    mesh.onReceive( []( 
            uint32_t aFrom, 
            String & aMsg
            )
        {
        Serial.printf( "onReceive - aFrom: 0x%X, %s\n",
                aFrom, aMsg.c_str() );
        } );

    mesh.onReceive( []( 
            String & aFrom, 
            String & aMsg
            )
        {
        Serial.printf( "onReceive - aFrom: %s, %s\n",
                aFrom.c_str(), aMsg.c_str() );
        } );

    mesh.onChangedConnections( []() 
        {
        SimpleList< uint32_t > nodes;
        nodes = mesh.getNodeList();

        Serial.printf( "\n(onChangedConnections) Num Nodes: %d\n", nodes.size() );
        Serial.println( "Connection List:" );
        SimpleList<uint32_t>::iterator n = nodes.begin();
        while( n != nodes.end() )
            {
            Serial.printf( "%u(0x%X)", *n, *n );
            n ++;
            }
        Serial.println( "" );
        } );

    }

void setup(
        ) 
    {
    int rc;
    int cnt;
    int cntDown;
    int routerCount = 0;

    stTime_g = millis();

    strcpy( mac, WiFi.macAddress().c_str() );

    Serial.begin( 115200 );
    delay( 1000 );
    
    for( cntDown = 5; cntDown > 0; cntDown -- )
        {
        Serial.printf( "(%d) \r", cntDown );
        delay( 1000 );
        }
    Serial.println( "" ); 

    /*
    ** First attempt to connect to one of the configured wifi networks.
    */
    WiFiInfo * wi;

    Serial.printf( "%s, Attempt Connect WiFi\r\n", mac );
    delay( 100 );

    for( wi = wifiInfo_g; wi->ssid != NULL; wi ++ )
        {
        routerCount ++;
        Serial.printf( "ssid: %s\r\n", wi->ssid );

        WiFi.begin( wi->ssid, wi->passwd );
        for( cnt = 0; (cnt < 100) && (WiFi.status() != WL_CONNECTED); cnt ++ )
            {
            delay( 100 );
            Serial.print( '.' );
            }
        Serial.printf( "\nDone(RouterCount: %d)(%d)\n", routerCount, cnt );

        if ( WiFi.status() == WL_CONNECTED )
            {
            break;
            }
        }

#ifdef USE_OTA
    if ( WiFi.status() == WL_CONNECTED )
        {
        wifi_g = wi;

        _checkUpdate();
        }
#endif

    _setupMesh();
    }

void loop(
        ) 
    {
    mesh.update();
    }
