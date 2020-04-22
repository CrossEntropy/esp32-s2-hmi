json = require('json')
dump = require('dump')
wifi = require('wifi')

print(dump.table(sys.info()))
if (not wifi.start_sta('esp-office-2.4G', '1qazxsw2')) then
    print('Connect to AP and log in to http://192.168.1.1 and configure router information')
    wifi.start_ap('ESP_LUA', '')
end
httpd.start('clock')
print(dump.table(net.info()))
assert(sys.sntp('ntp1.aliyun.com'))
print(os.date("%Y-%m-%d %H:%M:%S"))
local info = {}
local city_sub = 'sh_temp'
local location = web.rest('GET', 'http://pv.sohu.com/cityjson')
if (location) then
    local location_json = string.match(location, '({.*});')
    local location_t = json.decode(location_json)
    print (dump.table(location_t))
    local cid_pre = string.sub(location_t.cid, 1, 3)
    if (cid_pre == '310') then
        city_sub = 'sh_temp'
    elseif (cid_pre == '360') then
        city_sub = 'nc_temp'
    end
    info.location = location_t
end

local mqtt_connected = false
local last_time = os.time()
mqtt.start('mqtt://mqtt.emake.run')
while (1) do
    local handle = mqtt.run()
    if (handle) then
        if (handle.event == 'MQTT_EVENT_DATA') then
            if (handle.topic == city_sub) then
                local display = os.date("%Y-%m-%d")
                local t = json.decode(handle.data)
                local str = string.format('The weather in %s is %s with a temperature of %s.', t.results[1].location.name, t.results[1].now.text, t.results[1].now.temperature)
                local shares = web.rest('GET', 'http://hq.sinajs.cn/list=sh688018')
                if (shares) then
                    local shares_json = '['..string.match(string.gsub(shares, ',', '\",\"'), '=(.*);')..']'
                    local shares_t = json.decode(shares_json)
                    info.shares = shares_t
                end
                info.clock = os.clock()
                info.date = os.date("%Y-%m-%d %H:%M:%S")
                info.info = sys.info()
                print(json.encode(info))
                mqtt.pub('box', json.encode(info), 0)
            end
        elseif (handle.event == 'MQTT_EVENT_CONNECTED') then
            mqtt_connected = true
            mqtt.sub(city_sub, 0)
        elseif (handle.event == 'MQTT_EVENT_DISCONNECTED') then
            mqtt_connected = false
        end
    end

    if (not handle) then
        sys.yield()
    end
end