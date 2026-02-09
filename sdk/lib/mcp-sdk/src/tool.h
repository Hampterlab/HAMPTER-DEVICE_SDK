#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// REPLACE WHOLE ObservationBuilder WITH THIS
struct ObservationBuilder {
  StaticJsonDocument<2048> doc;
  JsonArray assets;
  ObservationBuilder(){
    doc["type"] = "device.observation";
    doc["ok"] = false;
    JsonObject res = doc.createNestedObject("result");
    res["text"] = "";                         // bridge expects this
    assets = res.createNestedArray("assets"); // and this
  }
  void setRequestId(const String& rid){ doc["request_id"]=rid; }
  void error(const char* code,const char* msg){
    doc["ok"]=false;
    JsonObject er=doc.createNestedObject("error");
    er["code"]=code; er["message"]=msg;
  }
  void setText(const char* text){ doc["result"]["text"]=text; }
  JsonObject addAsset(){ return assets.createNestedObject(); }
  void success(const char* text){ doc["ok"]=true; setText(text); }
  String toJson() const { String s; serializeJson(doc,s); return s; }
};

class WebServer;

struct ITool{
  virtual ~ITool(){}
  virtual bool init()=0;
  virtual const char* name() const =0;
  virtual void describe(JsonObject& tool)=0;
  virtual bool invoke(JsonObjectConst args, ObservationBuilder& out)=0;
  // optional, default no-op
  virtual void register_http(WebServer& srv) {}
};