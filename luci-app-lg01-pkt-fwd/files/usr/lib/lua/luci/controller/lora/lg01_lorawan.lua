
module("luci.controller.lora.lg01_lorawan",package.seeall)

function index()
    entry({"admin","network","lora"}, cbi("lora/lg01_gateway"),_("SCG (LG01) LoRaWAN"),99).index=true
end
