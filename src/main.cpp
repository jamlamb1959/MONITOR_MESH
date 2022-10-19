#define USE_OTA

#include <Arduino.h>

#include <painlessMesh.h>

#ifdef USE_OTA
#include <ESP32httpUpdate.h>

#define PROG        "firmware/espressif32/M5Stack/firmware"

#define LED_BUILTIN 15
#endif

#include <Wire.h>
#include <SPI.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BNO055.h>
#include <utility/imumaths.h>

#include <Preferences.h>
#include <M5Stack.h>
#include <math.h>
#include <M5_BMM150.h>
#include <M5_BMM150_DEFS.h>

#include <WiFi.h>
#include <esp_wifi.h>

#define MESH_SSID     "pharmdata"
#define MESH_PASSWORD "4026892842"
#define MESH_PORT     5555

#define RPTINV      1
#define RUNTIME     30
#define SLEEPINV    300

static bool connected_g = false;
static std::list< uint32_t > gwAddr_g;
RTC_DATA_ATTR unsigned int rptCnt_g;

static Preferences prefs;

static struct bmm150_dev bmm150;

static bmm150_mag_data _mag_offset;
static bmm150_mag_data _mag_max;
static bmm150_mag_data _mag_min;

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

static uint8_t broadcast[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

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
        M5.Lcd.setCursor( 0, 200 );
        M5.Lcd.printf( "%u/%u(%%%d.%d)\r", aDone, aTotal, pct10 / 10, pct10 % 10 );
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
static uint64_t sleepTime_g;
static uint64_t stTime_g;

static int8_t _i2c_read(
        uint8_t aDevId,
        uint8_t aRegAddr,
        uint8_t * aBuf,
        uint16_t aBufLen
        )
    {
    if ( M5.I2C.readBytes( aDevId, aRegAddr, aBufLen, aBuf ) )
        {
        return BMM150_OK;
        }

    return BMM150_E_DEV_NOT_FOUND;
    }

static int8_t _i2c_write(
        uint8_t aDevId,
        uint8_t aRegAddr,
        uint8_t * aBuf,
        uint16_t aBufLen
        )
    {
    if ( M5.I2C.writeBytes( aDevId, aRegAddr, aBuf, aBufLen ) )
        {
        return BMM150_OK;
        }

    return BMM150_E_DEV_NOT_FOUND;
    }

            
static int8_t _init_bmm150(
        )
    {
    int8_t rc;

    bmm150.dev_id = 0x10;
    bmm150.intf = BMM150_I2C_INTF;
    bmm150.read =_i2c_read;
    bmm150.write =_i2c_write;
    bmm150.delay_ms = delay;

    _mag_max.x = -2000;
    _mag_max.y = -2000;
    _mag_max.z = -2000;

    _mag_min.x = 2000;
    _mag_min.y = 2000;
    _mag_min.z = 2000;

    rc = bmm150_init( &bmm150 );

    bmm150.settings.pwr_mode = BMM150_NORMAL_MODE;
    rc |= bmm150_set_op_mode( &bmm150 );
   
    bmm150.settings.preset_mode = BMM150_PRESETMODE_ENHANCED;
    rc |= bmm150_set_presetmode( &bmm150 );

    return rc;
    }


static void _offset_load_bmm150(
        )
    {
    if ( prefs.begin( "bmm150", true ) )
        {
        prefs.getBytes( "offset", (uint8_t *)&_mag_offset, sizeof( _mag_offset ) );
        prefs.end();
        Serial.println( "bmm150 pref load ok." );
        }
    else
        {
        Serial.println( "bmm150 pref load failed." );
        }
    }

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
            if ( aMsg == "IM" )
                {
                bool found = false;

                for( uint32_t n : gwAddr_g )
                    {
                    if ( aFromAddr == n )
                        {
                        found = true;
                        }
                    }

                if ( !found )
                    {
                    gwAddr_g.push_back( aFromAddr );
                    /*
                    ** extend the sleep time if a gateway responds.
                    */
                    sleepTime_g = millis() + (RUNTIME * 1000);
                    M5.Lcd.clearDisplay();
                    M5.Lcd.setCursor( 0, 0 );
                    }
                else
                    {
                    Serial.printf( "(0x%X) duplicate IM\n", aFromAddr );
                    }

                Serial.printf( "gwAddr_g.size(): %u\n", gwAddr_g.size() );
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

    mesh.setDebugMsgTypes( CONNECTION | ERROR | REMOTE );
    // mesh.setDebugMsgTypes( ERROR | MESH_STATUS | DEBUG | COMMUNICATION | CONNECTION );

    mesh.init( MESH_SSID, MESH_PASSWORD, &userScheduler, MESH_PORT );

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
        Serial.println( "\ngwAddr_g:" );
        for( uint32_t n : gwAddr_g )
            {
            Serial.printf( "(0x%X)", n );
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
        Serial.println( "\ngwAddr_g:" );
        for( uint32_t gw : gwAddr_g )
            {
            Serial.printf( "(0x%X)", gw );
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

        M5.Lcd.printf( "\n(onChangedConnections) Num Nodes: %d\n", nodes.size() );
        M5.Lcd.println( "Connection List:" );
        SimpleList<uint32_t>::iterator n = nodes.begin();
        while( n != nodes.end() )
            {
            M5.Lcd.printf( "%u(0x%X)", *n, *n );
            n ++;
            }
        M5.Lcd.println( "\n(before) gwAddr_g:" );

        SimpleList<uint32_t>::iterator it;
        std::list< uint32_t >::iterator iit;
        
        for( iit = gwAddr_g.begin(); iit != gwAddr_g.end(); )
            {
            M5.Lcd.printf( "(0x%X)", *iit );

            for( it = nodes.begin(); it != nodes.end(); it ++ )
                {
                if ( *iit == *it )
                    {
                    break;
                    }
                }

            if ( it == nodes.end() )
                {
                M5.Lcd.printf( "Remove: %u\n", *iit );
                gwAddr_g.erase( iit );
                break;
                }
            else
                {
                iit ++;
                }
            }

        M5.Lcd.println( "\n(after) gwAddr_g:" );
        for( uint32_t gw : gwAddr_g )
            {
            M5.Lcd.printf( "(0x%X)", gw );
            }

        M5.Lcd.println( "" );
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


    M5.begin( true, true, true, true );
    M5.Lcd.printf( "%s(%s %s)\n", __FILE__, __DATE__, __TIME__ );

    Wire.begin( 21, 22, 400000UL );

    M5.Power.begin();
    if ( M5.Power.canControl() )
        {
        batLevel_g = M5.Power.getBatteryLevel();
        Serial.printf( "getBattryLevel(): %d\n", batLevel_g );
        M5.Lcd.printf( "getBattryLevel(): %d\n", batLevel_g );
        }
    
    byte address;
    byte err;

    for( address = 1; address < 127; address ++ )
        {
        Wire.beginTransmission( address );
        err = Wire.endTransmission();
        if ( err == 0 )
            {
            M5.Lcd.printf( "(%X) ", address );
            Serial.printf( "(%X) ", address );
            }
        }
    M5.Lcd.println( "" );
    Serial.println( "" );

    rc = M5.IMU.Init();
    M5.Lcd.printf( "IMU.Init(): %d\n", rc );
    Serial.printf( "IMU.Init(): %d\n", rc );

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
        M5.Lcd.printf( "ssid: %s\n", wi->ssid );

        WiFi.begin( wi->ssid, wi->passwd );
        for( cnt = 0; (cnt < 100) && (WiFi.status() != WL_CONNECTED); cnt ++ )
            {
            delay( 100 );
            M5.Lcd.print( '.' );
            }
        M5.Lcd.printf( "\nDone(RouterCount: %d)(%d)\n", routerCount, cnt );

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

#ifdef ESP8266
    WiFi.mode( WIFI_STA );
    WiFi.disconnect( false );
#else
    WiFi.mode( WIFI_MODE_STA );
    WiFi.disconnect( false, true );
#endif

    rc = _init_bmm150();
    M5.Lcd.printf( "_init_bmm150: %d(%d)\n", rc, BMM150_OK );
    
    _offset_load_bmm150();

    _setupMesh();
    sleepTime_g = millis() + (RUNTIME * 1000);
    }

unsigned long nxtRpt_g = 0;

static void _offset_save(
        )
    {
    prefs.begin( "bmm150", false );
    prefs.putBytes( "offset", (uint8_t *) &_mag_offset, sizeof( _mag_offset ) );
    prefs.end();

    }

static void _calibrate_bmm150(
        uint32_t aTmo
        )
    {
    uint32_t endTime = millis() + aTmo;
    uint32_t samples = 0;

    Serial.println( "Start calibration" );

    while ( millis() < endTime )
        {
        samples ++;

        bmm150_read_mag_data( &bmm150 );

        if ( bmm150.data.x < _mag_min.x )
            {
            _mag_min.x = bmm150.data.x;
            }

        if ( bmm150.data.x > _mag_max.x )
            {
            _mag_max.x = bmm150.data.x;
            }

        if ( bmm150.data.y < _mag_min.y )
            {
            _mag_min.y = bmm150.data.y;
            }

        if ( bmm150.data.y > _mag_max.y )
            {
            _mag_max.y = bmm150.data.y;
            }

        if ( bmm150.data.z < _mag_min.z )
            {
            _mag_min.z = bmm150.data.z;
            }

        if ( bmm150.data.z > _mag_max.z )
            {
            _mag_max.z = bmm150.data.z;
            }

        delay( 100 );
        }

    _mag_offset.x = (_mag_max.x + _mag_min.x) / 2;
    _mag_offset.y = (_mag_max.y + _mag_min.y) / 2;
    _mag_offset.z = (_mag_max.z + _mag_min.z) / 2;

    _offset_save();

    Serial.printf( "calibrate finished.  Sample: %u\n", samples );
    Serial.printf( "x - min: %.1f, max: %.1f(%.1f)\n", _mag_min.x, _mag_max.x, (_mag_max.x - _mag_min.x) );
    Serial.printf( "y - min: %.1f, max: %.1f(%.1f)\n", _mag_min.y, _mag_max.y, (_mag_max.y - _mag_min.y) );
    Serial.printf( "z - min: %.1f, max: %.1f(%.1f)\n", _mag_min.z, _mag_max.z, (_mag_max.z - _mag_min.z) );
    }


void loop(
        ) 
    {
    uint8_t buf[ 200 ];

    float heading;
    float tf;

    mesh.update();

    if ( millis() > nxtRpt_g )
        {
        nxtRpt_g = millis() + (RPTINV * 1000);

        M5.IMU.getTempData( &tf );
        tf = ((tf * 9.0)/5.0) + 32.0;

        bmm150_read_mag_data( &bmm150 );
        heading = atan2( bmm150.data.x - _mag_offset.x, bmm150.data.y - _mag_offset.y) * 180.0 / M_PI;

        sprintf( (char *) buf, 
                "{\"MAC\":\"%s\",\"RPTCNT\":%u,\"b\":%d,\"heading\":%.1f,\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"t\":%.1f}", 
                mac, rptCnt_g ++, batLevel_g, heading, (bmm150.data.x - _mag_offset.x), 
                        (bmm150.data.y - _mag_offset.y), (bmm150.data.z - _mag_offset.z), tf );

        if ( gwAddr_g.size() == 0 )
            {
            mesh.sendBroadcast( String( (char *)  buf ) );
            mesh.sendBroadcast( String( "GW" ) );
            Serial.printf( "sendBroadcast - %s\n", (char *) buf );
            }
        else
            {
            for( uint32_t n : gwAddr_g )
                {
                Serial.printf( "sendSingle - (0x%X) - %s\n",
                        n, (char *) buf );
                mesh.sendSingle( n, String( (char *) buf ) );
                }
            }

        M5.Lcd.println( (char *) buf );
        }

    M5.update();

/*
    bmm150_read_mag_data( &bmm150 );
    Serial.printf( "heading: %.1f\n", heading );
    Serial.printf( "MAG X: %.1f\tMAG Y: %.1f\tMAG Z: %.1f\n", bmm150.data.x, bmm150.data.y, bmm150.data.z );
    Serial.printf( "MID X: %.1f\tMID Y: %.1f\tMID Z: %.1f\n", _mag_offset.x, _mag_offset.y, _mag_offset.z );
*/

    if ( M5.BtnA.wasPressed() )
        {
        _calibrate_bmm150( 10000 );
        }
    delay( 100 );    

    if ( (sleepTime_g != 0) && (millis() > sleepTime_g) )
        {
        unsigned long et;

        sleepTime_g = 0;
        sprintf( (char *) buf, 
                "{\"MAC\":\"%s\",\"RUNTIME\":%lu,\"SLEEP\":%d}", 
                mac, (unsigned long) ((millis() - stTime_g) / 1000), SLEEPINV );

        if ( gwAddr_g.size() == 0 )
            {
            mesh.sendBroadcast( String( (char *)  buf ) );
            Serial.printf( "sendBroadcast - %s\n", (char *) buf );
            }
        else
            {
            for( uint32_t n : gwAddr_g )
                {
                Serial.printf( "sendSingle - (0x%X) - %s\n",
                        n, (char *) buf );
                mesh.sendSingle( n, String( (char *) buf ) );
                }
            }

        Serial.printf( "\n%s\n", buf );

        et = millis() + 5000;
        
        while ( millis() < et )
            {
            M5.update();
            mesh.update();
            delay( 100 );
            }

        M5.Power.deepSleep( SLEEP_SEC( SLEEPINV ) );
        }


  }
