/* web_ui_v2 on-demand chunk: the Quick Setup wizard (#/setup[/<screen>]).
   Loaded by the PAGES.setup stub in index.html the first time the page
   opens; shares the page's globals (h, ic, api, tryGet, toast, modal,
   banner, note, chip, page, conn, store, expectReboot, apDefaultPassword,
   vehicleProfilePicker, profileToPids). Design: TASK_quick_setup.md and the
   mockup artifact (2026-10-01).

   Ten screens in two halves. Screens 1 to 6 collect everything that is
   reboot-to-apply (AP password, home WiFi, MQTT or Home Assistant) and
   screen 6 saves it in ONE restart; the phone changes network there, so
   screens 7 to 10 rebuild from device state and resume from the URL
   (#/setup/checks), never from browser storage. */
(function(){
"use strict";

const STEPS=[
  {id:"safety",t:"Safety first",g:"Before the restart"},
  {id:"use",t:"How you use WiCAN"},
  {id:"details",t:()=>W.use==="mqtt"?"MQTT broker":W.use==="wifi"?"WiFi only":"Home Assistant"},
  {id:"ap",t:"Access point"},
  {id:"wifi",t:"Home WiFi"},
  {id:"review",t:"Review and restart"},
  {id:"reconnect",t:"Reconnect",g:"After the restart"},
  {id:"checks",t:"Checks"},
  {id:"vehicle",t:"Your vehicle"},
  {id:"power",t:"Battery and sleep"},
  {id:"polling",t:"Reading the car"},
  {id:"done",t:"Done"},
];
const IDS=STEPS.map(s=>s.id);
const PROTO={"6":"CAN 11-bit 500 kbit/s","7":"CAN 29-bit 500 kbit/s","8":"CAN 11-bit 250 kbit/s","9":"CAN 29-bit 250 kbit/s"};
/* how a car wants its legislated data asked (the vehicle store's `dialect`):
   only the newer one is worth a word, OBD-II is what everybody expects */
const DIALECT={uds:"OBD on UDS (ISO 27145)",j1939:"J1939"};
const HA_REPO="https://github.com/jay-oswald/ha-wican";
const SKIP_KEY="wican-setup-skip";

/* wizard state: survives in-page navigation and re-routes (module scope),
   not a reload on another address (the second half reads the device) */
const W={cur:null,use:"ha",agree:[false,false,false],haInstalled:false,
  apPw:"",apPw2:"",showPw:false,
  ssid:"",wifiAuth:"",wifiPw:"",manual:false,nets:null,scanErr:null,
  mqtt:{url:"",user:"",pw:"",prefix:"",period:5},
  applied:false,noVeh:false,
  car:null,noProfile:false,proto:"0",plugged:false,ignition:false,
  scan:"idle",scanStatus:null,scanResult:null,stdSel:new Set(),
  vehPhase:null,veh:null,vehName:"",profChoice:null,profCar:null,profList:undefined,
  poll:{rate:5,rateCustom:3,minEvent:1,pause:"sleep",pauseV:12.5,pauseAll:false,std:true,specific:true,custom:true,dtc:false,dtcMin:60},pollSeeded:false,
  /* step 10, battery and sleep: the live trace, the two readings, the chosen pair */
  pwr:{phase:"idle",trace:[],base:null,charging:null,resting:null,tC:0,tD:0,tR:0,sleep:13.1,wake:13.2,touched:false,narrow:false,measured:false,skipped:false,misses:0}};
/* device documents fetched while the wizard is open */
const D={wifi:null,mqtt:null,dest:null,autopid:null,autopidSchema:null,cfg:null,vehicle:null,vehicles:null,webhook:null,wifiStatus:null,sleep:undefined,sleepSchema:undefined};

const CSS=`
.qs{display:grid;grid-template-columns:232px minmax(0,1fr);background:var(--surface);border:1px solid var(--border);border-radius:12px;box-shadow:var(--shadow);overflow:hidden;min-height:560px}
.qs-rail{background:var(--surface-2);border-right:1px solid var(--border);padding:18px 14px 18px 18px;display:flex;flex-direction:column;gap:4px}
.qs-rail .grp{font-size:10.5px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--text-3);padding:10px 0 6px 6px}
.qs-step{display:grid;grid-template-columns:22px 1fr;gap:10px;align-items:center;padding:7px 8px;border-radius:9px;border:0;background:none;text-align:left;cursor:pointer;color:var(--text-2);font-weight:600;font-size:13px;font-family:inherit}
.qs-step:hover:not(:disabled){background:var(--surface-3)}
.qs-step:disabled{cursor:default}
.qs-step .n{width:22px;height:22px;border-radius:50%;border:2px solid var(--border-strong);display:grid;place-items:center;font-size:11px;font-weight:700;color:var(--text-3);background:var(--surface)}
.qs-step .n svg{width:12px;height:12px}
.qs-step.cur{background:var(--primary-tint);color:var(--primary)}
.qs-step.cur .n{border-color:var(--primary);color:var(--primary)}
.qs-step.done{color:var(--text)}
.qs-step.done .n{background:var(--success);border-color:var(--success);color:#fff}
.qs-rail .div{height:1px;background:var(--border);margin:8px 6px}
.qs-rail .restart{display:flex;align-items:center;gap:8px;font-size:11.5px;color:var(--warning);font-weight:700;padding:4px 6px 2px}
.qs-rail .restart svg{width:14px;height:14px;flex:none}
.qs-screen{padding:26px 30px 22px;display:flex;flex-direction:column;gap:18px;min-width:0}
.qs-screen h2{font-size:20px;font-weight:800;letter-spacing:-.015em;margin:0}
.qs-lead{color:var(--text-2);max-width:62ch;margin:0}
.qs-body{display:flex;flex-direction:column;gap:14px;flex:1}
.qs-foot{display:flex;align-items:center;gap:10px;flex-wrap:wrap;border-top:1px solid var(--border);padding-top:16px;margin-top:4px}
.qs-foot .ghost{color:var(--text-3);font-size:12.5px}
.qs-agree{display:flex;gap:11px;align-items:flex-start;padding:12px 14px;border:1px solid var(--border);border-radius:10px;cursor:pointer;background:var(--surface)}
.qs-agree:has(input:checked){border-color:color-mix(in srgb,var(--success) 50%,transparent);background:var(--success-tint)}
.qs-agree input{width:18px;height:18px;margin:2px 0 0;flex:none;accent-color:var(--success)}
.qs-agree b{display:block;margin-bottom:2px}
.qs-agree span{color:var(--text-2);font-size:13px;line-height:1.45}
.qs-tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:12px}
.qs-tile{position:relative;text-align:left;padding:16px 16px 14px;border:1px solid var(--border);border-radius:12px;background:var(--surface);cursor:pointer;display:flex;flex-direction:column;gap:8px;min-height:150px;font-family:inherit;color:var(--text)}
.qs-tile:hover:not(.later){border-color:var(--border-strong)}
.qs-tile.sel{border-color:var(--primary);box-shadow:0 0 0 3px var(--focus);background:var(--primary-tint)}
.qs-tile.later{opacity:.6;cursor:default}
.qs-tile .ticon{width:34px;height:34px;border-radius:9px;display:grid;place-items:center;background:var(--surface-3);color:var(--primary)}
.qs-tile.sel .ticon{background:var(--surface)}
.qs-tile .ticon svg{width:18px;height:18px}
.qs-tile h3{font-size:14.5px;font-weight:700;margin:0}
.qs-tile p{color:var(--text-2);font-size:12.5px;line-height:1.45;margin:0}
.qs-tile .tag{position:absolute;top:12px;right:12px}
.qs-row{display:grid;grid-template-columns:180px minmax(0,1fr);gap:5px 20px;align-items:start;padding:7px 0}
.qs-row>label{color:var(--text-2);font-weight:600;font-size:13.5px;padding-top:9px;line-height:1.4}
.qs-row .ctl{min-width:0;display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.qs-row .ctl input{max-width:360px}
.qs-row .help{grid-column:2;color:var(--text-3);font-size:12px;line-height:1.45}
.qs-row .help.err{color:var(--danger)}
.qs-strength{height:6px;border-radius:3px;background:var(--surface-3);overflow:hidden;width:360px;max-width:100%;margin-bottom:4px}
.qs-strength i{display:block;height:100%;background:var(--warning);transition:width .2s}
.qs-strength.good i{background:var(--success)}
.qs-nets{border:1px solid var(--border);border-radius:10px;overflow:hidden}
.qs-net{display:grid;grid-template-columns:1fr 60px 90px;gap:12px;align-items:center;padding:10px 14px;border-top:1px solid var(--border);cursor:pointer;background:var(--surface)}
.qs-net:first-child{border-top:0}
.qs-net:hover{background:var(--surface-2)}
.qs-net.sel{background:var(--primary-tint)}
.qs-net .ssid{font-weight:600;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.qs-net.sel .ssid::after{content:"Selected";margin-left:10px;font-size:11px;color:var(--primary);font-weight:700}
.qs-net .bars{font-family:var(--mono);color:var(--primary);letter-spacing:1px}
.qs-details{border:1px solid var(--border);border-radius:10px;padding:10px 14px;background:var(--surface-2)}
.qs-details summary{cursor:pointer;font-weight:600;color:var(--text-2);font-size:13px}
.qs-details[open] summary{margin-bottom:8px}
.qs-kv{display:grid;grid-template-columns:170px 1fr;gap:8px 18px;margin:0}
.qs-kv dt{color:var(--text-3);font-weight:600;font-size:12.5px;padding-top:2px}
.qs-kv dd{margin:0;display:flex;gap:8px;align-items:center;flex-wrap:wrap;min-width:0}
.qs-steps{display:flex;flex-direction:column;gap:12px;counter-reset:s;margin:0;padding:0}
.qs-steps li{display:grid;grid-template-columns:26px minmax(0,1fr);gap:2px 12px;align-items:start;list-style:none}
.qs-steps li::before{counter-increment:s;content:counter(s);grid-column:1;grid-row:1;width:24px;height:24px;border-radius:50%;background:var(--primary);color:var(--on-primary);display:grid;place-items:center;font-size:12px;font-weight:700;margin-top:1px}
.qs-steps li>b{grid-column:2;grid-row:1;display:block;padding-top:3px}
.qs-steps li>span{grid-column:2;grid-row:2;display:block;color:var(--text-2);font-size:13px;line-height:1.45}
.qs-check{display:grid;grid-template-columns:32px minmax(0,1fr) auto;gap:12px;align-items:center;padding:12px 14px;border:1px solid var(--border);border-radius:10px;background:var(--surface)}
.qs-check .ci{width:30px;height:30px;border-radius:8px;display:grid;place-items:center;background:var(--surface-3);color:var(--text-2)}
.qs-check .ci svg{width:16px;height:16px}
.qs-check b{display:block;font-size:13.5px}
.qs-check span.d2{color:var(--text-2);font-size:12.5px;display:block}
.qs-check.ok .ci{background:var(--success-tint);color:var(--success)}
.qs-check.warn .ci{background:var(--warning-tint);color:var(--warning)}
.qs-check.crit .ci{background:var(--danger-tint);color:var(--danger)}
.qs-spin{width:18px;height:18px;border:2.5px solid var(--border-strong);border-top-color:var(--primary);border-radius:50%;animation:qsspin .8s linear infinite;flex:none;margin:0 auto}
@keyframes qsspin{to{transform:rotate(360deg)}}
@media (prefers-reduced-motion:reduce){.qs-spin{animation:none}}
.qs-link{display:flex;flex-direction:column;gap:10px;padding:18px 20px;border:1px solid color-mix(in srgb,var(--primary) 35%,transparent);border-radius:12px;background:var(--primary-tint)}
.qs-link .url{font-family:var(--mono);font-size:15px;font-weight:600;word-break:break-all;color:var(--text)}
.qs-link .acts{display:flex;gap:8px;flex-wrap:wrap}
.qs-sub{display:flex;align-items:center;gap:10px;margin-top:6px}
.qs-sub h3{font-size:13px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:var(--text-3);margin:0}
.qs-sub::after{content:"";flex:1;height:1px;background:var(--border)}
.qs-pick{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.qs-pick .sel{display:inline-flex;align-items:center;gap:8px;padding:7px 12px;border:1px solid var(--border);border-radius:9px;background:var(--surface-2);font-weight:600}
.qs-pick .sel svg{width:16px;height:16px;flex:none;color:var(--primary)}
.qs-radios{display:flex;flex-direction:column;gap:6px}
.qs-radios label{display:flex;gap:9px;align-items:center;font-size:13.5px;cursor:pointer}
.qs-radios input{width:16px;height:16px;margin:0;accent-color:var(--primary)}
.qs-pids{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:6px 14px;max-height:200px;overflow:auto;padding:8px 2px}
.qs-pids label{display:flex;gap:8px;align-items:center;font-size:12.5px;color:var(--text);cursor:pointer}
.qs-pids label code{color:var(--text-3);font-size:12px}
.qs-pids input{width:15px;height:15px;margin:0;accent-color:var(--primary)}
.qs-next{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px}
.qs-next a{display:flex;flex-direction:column;gap:3px;padding:12px 14px;border:1px solid var(--border);border-radius:10px;text-decoration:none;color:var(--text);background:var(--surface)}
.qs-next a b{color:var(--primary)}
.qs-next a span{font-size:12.5px;color:var(--text-2)}
.qs-inline{display:flex;gap:8px;flex-wrap:wrap;align-items:center;font-size:12.5px;color:var(--text-3)}
/* step 10: the live battery card, its trace and the two threshold rows */
.qs-live{display:grid;grid-template-columns:minmax(150px,auto) minmax(0,1fr);gap:14px 22px;align-items:start;padding:14px 16px;border:1px solid var(--border);border-radius:12px;background:var(--surface-2)}
.qs-live .big{font-family:var(--mono);font-size:34px;font-weight:600;line-height:1;letter-spacing:-.02em;font-variant-numeric:tabular-nums}
.qs-live .big small{font-size:15px;color:var(--text-3);margin-left:4px;font-weight:500}
.qs-live .lbl{font-size:11px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:var(--text-3);margin-bottom:6px}
.qs-live .st{margin-top:8px}
.qs-trace svg{width:100%;height:auto;display:block}
.qs-trace .g{stroke:var(--border);stroke-width:1}
.qs-trace .l{fill:none;stroke:var(--primary);stroke-width:2;stroke-linejoin:round;stroke-linecap:round}
.qs-trace .f{fill:var(--primary);opacity:.08}
.qs-trace .t{stroke-dasharray:4 4;stroke-width:1.5}
.qs-trace .t.sleep{stroke:var(--warning)}
.qs-trace .t.wake{stroke:var(--success)}
.qs-trace text{font-family:var(--mono);font-size:10.5px;fill:var(--text-3)}
.qs-trace text.sleep{fill:var(--warning)}
.qs-trace text.wake{fill:var(--success)}
.qs-trace .m{fill:var(--surface);stroke:var(--primary);stroke-width:2}
.qs-trace .m.e{fill:var(--primary)}
.qs-thr{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.qs-thr-row{display:flex;flex-direction:column;gap:6px;padding:12px 14px;border:1px solid var(--border);border-radius:10px;background:var(--surface)}
.qs-thr-row>b{display:flex;align-items:center;gap:8px;font-size:13.5px}
.qs-thr-row>b i{width:10px;height:3px;border-radius:2px;display:inline-block}
.qs-thr-row .val{font-family:var(--mono);font-size:22px;font-weight:600;font-variant-numeric:tabular-nums}
.qs-thr-row input[type=range]{width:100%;max-width:none;padding:0;border:0;background:none;accent-color:var(--primary)}
.qs-thr-row>span{color:var(--text-2);font-size:12.5px;line-height:1.45}
@media (max-width:860px){
  .qs{grid-template-columns:1fr}
  .qs-live,.qs-thr{grid-template-columns:1fr}
  .qs-rail{flex-direction:row;flex-wrap:wrap;border-right:0;border-bottom:1px solid var(--border);padding:12px}
  .qs-rail .grp,.qs-rail .div,.qs-rail .restart{display:none}
  .qs-step{grid-template-columns:22px auto;padding:5px 8px}
  .qs-screen{padding:20px 18px}
  .qs-row,.qs-kv{grid-template-columns:1fr}
  .qs-row .help{grid-column:1}
  .qs-net{grid-template-columns:1fr 50px 70px}
}`;

/* ---------- small helpers ---------- */
const deviceId=()=>(conn.info&&conn.info.device_id)||(conn.status&&conn.status.device_id)||"";
const apName=()=>deviceId()?"WiCAN_"+deviceId():"the WiCAN access point";
const mdnsHost=()=>deviceId()?"wican_"+deviceId()+".local":location.hostname;
const strip=o=>{const c={...(o||{})};delete c.degraded;delete c.pending_reboot;return c;};
const bars=r=>{const n=r>-55?4:r>-67?3:r>-78?2:1;return "▂▄▆█".slice(0,n).padEnd(4,"·");};
const netAuth=n=>String(n.auth_mode||n.auth||"").toUpperCase();
const isOpen=n=>!netAuth(n)||netAuth(n)==="OPEN";
const btn=(label,fn,cls,opt={})=>{const b=h("button",{class:"btn "+(cls||""),type:"button",disabled:!!opt.disabled,title:opt.title,onclick:fn},opt.icon?ic(opt.icon):null,label);return b;};
const check=(kind,icon,title,text,chipEl)=>h("div",{class:"qs-check "+(kind||"")},
  kind==="run"?h("span",{class:"qs-spin"}):h("span",{class:"ci"},ic(icon)),
  h("div",{},h("b",{},title),text?h("span",{class:"d2"},...(Array.isArray(text)?text:[text])):null),chipEl||h("span",{}));
const sub=t=>h("div",{class:"qs-sub"},h("h3",{},t));
const row=(label,ctl,help,err)=>h("div",{class:"qs-row"},h("label",{for:ctl.id||null},label),h("div",{class:"ctl"},ctl),help?h("div",{class:"help"+(err?" err":"")},help):null);
const steps=items=>h("ol",{class:"qs-steps"},...items.map(([b,s])=>h("li",{},h("b",{},b),h("span",{},...(Array.isArray(s)?s:[s])))));
function copyText(txt,b){
  const done=()=>{if(b){b.textContent="Copied";setTimeout(()=>{b.replaceChildren(ic("copy"),"Copy link");},1500);}toast("Copied","ok");};
  if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(txt).then(done).catch(()=>toast(txt,""));}
  else toast(txt,"");
}
function skipSetup(){try{sessionStorage.setItem(SKIP_KEY,"1");}catch(_){}location.hash="#/status";}

/* ---------- navigation ---------- */
let railEl=null,scrEl=null,timers=[];
function every(ms,fn){const t=setInterval(()=>{if(!scrEl||!scrEl.isConnected){clearInterval(t);return;}fn();},ms);timers.push(t);return t;}
function stopTimers(){timers.forEach(clearInterval);timers=[];}
function go(id){
  if(!IDS.includes(id))id="safety";
  W.cur=id;
  try{history.replaceState(null,"","#/setup/"+id);}catch(_){}
  paint();
}
function paintRail(){
  const curIdx=IDS.indexOf(W.cur);
  railEl.replaceChildren();
  let lastG=null;
  STEPS.forEach((s,i)=>{
    if(s.g&&s.g!==lastG){
      if(lastG){railEl.append(h("div",{class:"div"}),h("div",{class:"restart"},ic("power"),"WiCAN restarts, your phone changes network"));}
      railEl.append(h("div",{class:"grp"},s.g));lastG=s.g;}
    const k=i===curIdx?"cur":i<curIdx?"done":"";
    const label=typeof s.t==="function"?s.t():s.t;
    /* earlier steps can be revisited (Fix WiFi, change the broker); later ones wait their turn */
    railEl.append(h("button",{class:"qs-step "+k,type:"button",disabled:i>curIdx,"data-step":s.id,onclick:()=>{if(i<=curIdx)go(s.id);}},
      h("span",{class:"n"},k==="done"?ic("check"):String(i+1)),h("span",{},label)));
  });
}
function paint(){
  stopTimers();
  paintRail();
  const fn=SCREENS[W.cur]||SCREENS.safety;
  scrEl.replaceChildren(h("div",{class:"loading"},h("div",{class:"spinner"})));
  Promise.resolve().then(()=>fn()).then(el=>{if(el)scrEl.replaceChildren(el);})
    .catch(e=>scrEl.replaceChildren(banner("crit","alert","This step could not be shown: "+(e.message||e))));
}
const screen=(title,lead,body,foot)=>h("div",{},h("h2",{},title),lead?h("p",{class:"qs-lead"},lead):null,h("div",{class:"qs-body"},...body),foot?h("div",{class:"qs-foot"},...foot):null);
const back=id=>btn("Back",()=>go(id),"");
const grow=()=>h("span",{class:"grow"});

/* ---------- screens ---------- */
const SCREENS={};

SCREENS.safety=()=>{
  const items=[
    ["Private networks only","I will connect WiCAN only to a private WiFi network I control, such as my home network. Never a public, hotel, cafe, office or guest network."],
    ["Keep the access point password private","I will set my own access point password and will not share the WiCAN network with people I do not trust."],
    ["My vehicle, my responsibility","I will use diagnostic features such as clearing trouble codes only on vehicles I own or am authorised to work on."]];
  const next=btn("Continue",()=>go("use"),"pri",{disabled:!W.agree.every(Boolean)});
  const boxes=items.map((it,i)=>h("label",{class:"qs-agree"},
    h("input",{type:"checkbox",id:"qs-agree"+i,checked:W.agree[i],onchange:e=>{W.agree[i]=e.target.checked;next.disabled=!W.agree.every(Boolean);}}),
    h("div",{},h("b",{},it[0]),h("span",{},it[1]))));
  const fresh=apDefaultPassword();
  return screen("Keep your WiCAN private",
    "WiCAN sits on your car's diagnostic port and on your WiFi. Read these three rules before it goes online. Each one needs a tick.",
    [...boxes,note("","The web interface has no login by default. Anyone on the same private network can open it. You can add a password later under Settings.")],
    [btn(fresh?"Skip setup for now":"Exit setup",fresh?skipSetup:()=>{location.hash="#/status";},"gh sm"),grow(),next]);
};

SCREENS.use=()=>{
  const tile=(id,icon,title,body,tag)=>h("button",{class:"qs-tile"+(W.use===id?" sel":""),type:"button","data-use":id,onclick:()=>{W.use=id;paint();}},
    tag?h("span",{class:"tag chip ok"},tag):null,h("span",{class:"ticon"},ic(icon)),h("h3",{},title),h("p",{},body));
  const later=(icon,title,body)=>h("div",{class:"qs-tile later","aria-disabled":"true"},
    h("span",{class:"tag chip"},"Not in Quick Setup yet"),h("span",{class:"ticon"},ic(icon)),h("h3",{},title),h("p",{},body));
  const onlyWifi=h("label",{class:"qs-agree",style:"margin-top:4px"},
    h("input",{type:"radio",name:"qs-use",id:"qs-use-wifi",checked:W.use==="wifi",onchange:()=>{W.use="wifi";paint();}}),
    h("div",{},h("b",{},"Just put WiCAN on my WiFi for now"),h("span",{},"Skips the use case screen. You can run Quick Setup again later.")));
  return screen("How will you use WiCAN?","Pick the one that matters most. Everything else stays available in the full interface afterwards.",
    [h("div",{class:"qs-tiles"},
      tile("ha","home","Home Assistant","Live vehicle data as Home Assistant entities through the WiCAN integration. Nothing to type here: Home Assistant finds WiCAN on your network.","Most common"),
      tile("mqtt","cloud","My own MQTT broker","Publish vehicle data to Mosquitto, HiveMQ, Node-RED or any broker. You enter the broker address and login on the next screen."),
      later("plug","OBD app over WiFi or Bluetooth","Car Scanner, Torque and similar apps. Works out of the box on the access point; set up in Settings."),
      later("route","ABRP, HTTP endpoints, data logger","Data destinations and the SD card logger live under Automate and Logger.")),
     onlyWifi],
    [back("safety"),grow(),btn("Continue",()=>go(W.use==="wifi"?"ap":"details"),"pri")]);
};

SCREENS.details=async()=>{
  if(W.use==="wifi")return screen("WiFi only","Nothing to set up for this choice. The next two screens secure the access point and join your WiFi.",
    [note("","Run Quick Setup again from the sidebar whenever you want to add Home Assistant or an MQTT broker.")],
    [back("use"),grow(),btn("Continue",()=>go("ap"),"pri")]);
  if(W.use==="mqtt"){
    if(!D.mqtt){D.mqtt=strip(await tryGet("/api/settings/mqtt_manager"));
      if(!W.mqtt.url&&D.mqtt.url)W.mqtt.url=D.mqtt.url;
      if(!W.mqtt.user&&D.mqtt.username)W.mqtt.user=D.mqtt.username;
      if(!W.mqtt.prefix&&D.mqtt.topic_prefix)W.mqtt.prefix=D.mqtt.topic_prefix;}
    if(!D.dest){D.dest=strip(await tryGet("/api/settings/data_destinations"));
      const r=(D.dest.destinations||[]).find(d=>d.type==="mqtt"&&String(d.url||"").replace(/\s/g,"")==="~/autopid");
      if(r&&r.period_s)W.mqtt.period=r.period_s;}
    const hadPw=!!D.mqtt.enabled;
    const next=btn("Continue",()=>go("ap"),"pri",{disabled:!W.mqtt.url.trim()});
    const inp=(id,key,attrs={})=>h("input",{id,value:W.mqtt[key]||"",...attrs,oninput:e=>{W.mqtt[key]=key==="period"?Math.max(1,Math.min(3600,Number(e.target.value)||5)):e.target.value;next.disabled=!W.mqtt.url.trim();}});
    return screen("Your MQTT broker","WiCAN connects to the broker once it is on your WiFi. The connection is tested after the restart.",
      [row("Broker address",inp("qs-mq-url","url",{placeholder:"mqtt://broker.local:1883"}),"mqtt:// for plain, mqtts:// for TLS (public certificates work out of the box)."),
       row("Username",inp("qs-mq-user","user",{placeholder:"optional"})),
       row("Password",inp("qs-mq-pw","pw",{type:"password",placeholder:hadPw?"unchanged, type to replace":"optional",autocomplete:"new-password"})),
       row("Topic prefix",inp("qs-mq-prefix","prefix",{placeholder:"wican/"+(deviceId()||"<device id>")+" (default)"}),
         ["WiCAN publishes ",h("code",{},"<prefix>/status")," (online or offline) and the live vehicle data on ",h("code",{},"<prefix>/autopid"),"."]),
       row("Send vehicle data every",h("div",{class:"rowflex"},inp("qs-mq-period","period",{type:"number",min:1,max:3600,style:"max-width:110px"}),h("span",{},"seconds")),"Retained, so a dashboard that connects later still sees the last values.")],
      [back("use"),grow(),next]);
  }
  /* Home Assistant */
  const id=deviceId()||"<device id>";
  const copyBtn=btn("Copy repository link",()=>copyText(HA_REPO,null),"sm",{icon:"copy"});
  return screen("Install the WiCAN integration in Home Assistant","Do this part in Home Assistant, on any device, before or after WiCAN joins your WiFi. Nothing to type on WiCAN itself.",
    [h("label",{class:"qs-agree"},h("input",{type:"checkbox",id:"qs-ha-installed",checked:W.haInstalled,onchange:e=>{W.haInstalled=e.target.checked;}}),
       h("div",{},h("b",{},"The WiCAN integration is already installed in my Home Assistant"),h("span",{},"Then only step 5 applies."))),
     steps([
       ["Install HACS if you do not have it",["HACS is the community store for Home Assistant. Guide: ",h("code",{},"hacs.xyz/docs/use"),"."]],
       ["Add the WiCAN repository to HACS",["HACS > three-dot menu > Custom repositories. Repository ",h("code",{},HA_REPO),", type ",h("b",{},"Integration"),", Add."]],
       ["Download the integration",["Search HACS for ",h("b",{},"WiCAN"),", open it, press Download."]],
       ["Restart Home Assistant","Settings > System > Restart. Home Assistant asks for this after a download."],
       ["Add WiCAN when it is discovered",["After WiCAN joins your WiFi (next screens), Settings > Devices & services shows ",h("b",{},"Discovered: WiCAN "+id),". Press Add. If it does not appear, press Add Integration, search WiCAN and enter ",h("code",{},mdnsHost()),"."]]]),
     banner("info","info","Home Assistant then registers itself with WiCAN and receives status and vehicle data every 60 s, and right away when a value changes. Both must be on the same network: discovery uses mDNS, which home routers pass and guest networks do not."),
     h("div",{class:"qs-inline"},copyBtn,h("span",{},"The checks screen after the restart shows when Home Assistant has connected."))],
    [back("use"),grow(),btn("Continue",()=>go("ap"),"pri")]);
};

SCREENS.ap=()=>{
  const fresh=apDefaultPassword();
  const valid=pw=>pw.length>=8&&pw.length<=63&&pw!=="@meatpi#";
  const next=btn("Continue",()=>go("wifi"),"pri");
  const meter=h("div",{class:"qs-strength"},h("i",{}));
  const help1=h("div",{class:"help"}),help2=h("div",{class:"help err"});
  const type=()=>W.showPw?"text":"password";
  const pw=h("input",{id:"qs-ap-pw",type:type(),value:W.apPw,autocomplete:"new-password",placeholder:"8 to 63 characters"});
  const pw2=h("input",{id:"qs-ap-pw2",type:type(),value:W.apPw2,autocomplete:"new-password"});
  const show=btn(W.showPw?"Hide":"Show",()=>{W.showPw=!W.showPw;pw.type=pw2.type=type();show.textContent=W.showPw?"Hide":"Show";},"sm");
  const update=()=>{
    const v=W.apPw,ok=valid(v),match=ok&&v===W.apPw2;
    const score=Math.min(100,Math.round(v.length*6+(/[0-9]/.test(v)?12:0)+(/[^A-Za-z0-9]/.test(v)?14:0)+(/[A-Z]/.test(v)?10:0)));
    meter.className="qs-strength"+(score>60?" good":"");meter.firstChild.style.width=(v?Math.max(8,score):0)+"%";
    help1.textContent=v&&!ok?(v==="@meatpi#"?"That is the factory password.":"Use 8 to 63 characters."):"A phrase of three or four words is easy to remember and hard to guess.";
    help1.className="help"+(v&&!ok?" err":"");
    help2.textContent=W.apPw2&&!match?"The two passwords differ.":"";
    /* a device that already has its own password may keep it (both blank) */
    next.disabled=fresh?!match:!(match||(!v&&!W.apPw2));};
  pw.oninput=e=>{W.apPw=e.target.value;update();};pw2.oninput=e=>{W.apPw2=e.target.value;update();};
  update();
  return screen("Secure the access point",fresh?"The access point is how you are connected right now. Give it a password only you know.":"The access point already has your own password. Leave both fields blank to keep it, or set a new one.",
    [fresh?banner("warn","lock",h("b",{},apName()+" still has the factory password (@meatpi#)."),
        " Everyone who knows WiCAN knows it. WiCAN will not save WiFi settings until it is replaced.")
      :banner("ok","lock",h("b",{},apName()+" uses your own password."),"Nothing to do here unless you want a new one."),
     h("div",{class:"qs-row"},h("label",{for:"qs-ap-pw"},"New password"),h("div",{class:"ctl"},pw,show),h("div",{class:"help"},meter,help1)),
     h("div",{class:"qs-row"},h("label",{for:"qs-ap-pw2"},"Confirm password"),h("div",{class:"ctl"},pw2),help2),
     note("",["Your phone will ask for this new password the next time it joins ",h("b",{},apName()),". Write it down now."])],
    [back(W.use==="wifi"?"use":"details"),grow(),next]);
};

SCREENS.wifi=async()=>{
  if(!D.wifi)D.wifi=strip(await tryGet("/api/settings/wifi_manager"));
  if(!W.ssid&&D.wifi.sta_ssid)W.ssid=D.wifi.sta_ssid;
  const stored=D.wifi.sta_ssid||"";
  const list=h("div",{class:"qs-nets"});
  const count=chip("…","");
  const openWarn=h("div",{});
  const pwInp=h("input",{id:"qs-wifi-pw",type:"password",value:W.wifiPw,autocomplete:"off"});
  const pwLabel=h("label",{for:"qs-wifi-pw"},"Password");
  const pwHelp=h("div",{class:"help"});
  const next=btn("Continue",()=>go("review"),"pri");
  const selNet=()=>(W.nets||[]).find(n=>n.ssid===W.ssid);
  const update=()=>{
    const n=selNet();const open=n?isOpen(n):false;W.wifiAuth=n?netAuth(n):"";
    pwLabel.textContent="Password for "+(W.ssid||"the network");
    pwInp.disabled=open;
    const keep=W.ssid&&W.ssid===stored&&!W.wifiPw;
    pwHelp.replaceChildren(keep?"Blank keeps the password already stored for "+stored+". WiCAN treats this network as trusted: you can manage it from any device on it."
      :"WiCAN treats this network as trusted: you can manage it from any device on it.");
    openWarn.replaceChildren(open?banner("crit","alert",h("b",{},W.ssid+" has no password."),
      " Quick Setup does not join open networks: anyone nearby could reach WiCAN. Pick a protected network."):null);
    next.disabled=!W.ssid||open||!(W.wifiPw.length>=8||keep);
    list.querySelectorAll(".qs-net").forEach(el=>el.classList.toggle("sel",el.dataset.ssid===W.ssid));};
  pwInp.oninput=e=>{W.wifiPw=e.target.value;update();};
  const render=()=>{
    const scanning=W.nets===null;
    const nets=W.nets||[];
    count.replaceChildren(h("span",{class:"d"}),scanning?"Scanning":W.scanErr?"Scan failed":nets.length+" network"+(nets.length===1?"":"s")+" found");
    count.className="chip "+(scanning?"":W.scanErr?"crit":nets.length?"ok":"");
    list.replaceChildren(...(scanning?[h("div",{class:"empty"},h("span",{class:"qs-spin",style:"display:inline-block;vertical-align:middle;margin-right:8px"}),"Looking for networks nearby (a few seconds)")]
      :nets.length?nets.map(n=>h("div",{class:"qs-net"+(n.ssid===W.ssid?" sel":""),"data-ssid":n.ssid,role:"button",tabindex:"0",
        onclick:()=>{W.ssid=n.ssid;W.manual=false;update();},onkeydown:e=>{if(e.key==="Enter"||e.key===" "){e.preventDefault();W.ssid=n.ssid;update();}}},
        h("span",{class:"ssid",title:n.ssid},n.ssid),h("span",{class:"bars"},bars(n.rssi)),
        isOpen(n)?chip("Open","warn"):chip(netAuth(n).includes("WPA3")?"WPA2/3":"WPA2","")))
      :[h("div",{class:"empty"},W.scanErr?"Scan failed: "+W.scanErr:"No networks found. Scan again in a few seconds, or enter the name by hand below.")]));
    update();};
  /* Scanning from the access point flips the radio to AP+STA for the
     duration and can drop this very connection for a moment (bench
     2026-10-01: one answer in three came back empty or not at all): retry
     twice, quietly, before showing an empty list or an error. */
  const scan=async(attempt=0)=>{
    rescan.disabled=true;rescan.replaceChildren(h("span",{class:"qs-spin",style:"width:14px;height:14px;border-width:2px"}),attempt?"Scanning again":"Scanning");
    let nets=null,err=null;
    try{const r=await api("/api/wifi/scan");const raw=(r&&r.networks)||[];
      /* one row per name, strongest signal wins (mesh systems repeat names) */
      const best=new Map();for(const n of raw){if(!n.ssid)continue;const o=best.get(n.ssid);if(!o||(n.rssi||-999)>(o.rssi||-999))best.set(n.ssid,n);}
      nets=[...best.values()].sort((a,b)=>(b.rssi||-999)-(a.rssi||-999));}
    catch(e){err=e.message;}
    if((err||!nets.length)&&attempt<2&&list.isConnected){await new Promise(r=>setTimeout(r,2500));return scan(attempt+1);}
    W.nets=nets||[];W.scanErr=err;
    rescan.disabled=false;rescan.replaceChildren(ic("refresh"),"Scan again");render();};
  const rescan=btn("Scan again",scan,"sm",{icon:"refresh"});
  const manual=h("input",{id:"qs-man-ssid",placeholder:"exact name, case matters",value:W.manual?W.ssid:"",oninput:e=>{W.ssid=e.target.value.trim();W.manual=true;update();}});
  const el=screen("Join your home WiFi","Pick the network your phone, Home Assistant or broker are on. WiCAN joins it after the restart and stays an access point as well.",
    [h("div",{class:"rowflex",style:"flex-wrap:wrap"},count,grow(),rescan),list,openWarn,
     h("details",{class:"qs-details",open:W.manual||null},h("summary",{},"Network not listed? Enter it by hand"),
       row("Network name (SSID)",manual,"Hidden networks and 5 GHz-only names do not appear in the scan. WiCAN connects on 2.4 GHz.")),
     h("div",{class:"qs-row"},pwLabel,h("div",{class:"ctl"},pwInp),pwHelp)],
    [back("ap"),grow(),next]);
  if(W.nets)render();else{W.nets=null;render();scan();}
  return el;
};

SCREENS.review=()=>{
  const err=h("div",{});
  const save=btn("Save and restart",()=>applyAll(save,err),"pri",{icon:"power"});
  const useRow=W.use==="ha"?[chip("Ready for discovery","ok"),h("span",{},"Home Assistant registers itself after the restart")]
    :W.use==="mqtt"?[h("code",{},W.mqtt.url.trim()),h("span",{},"as "+(W.mqtt.user||"anonymous")+", vehicle data every "+W.mqtt.period+" s")]
    :[chip("Not now",""),h("span",{},"Run Quick Setup again to add one")];
  const apRow=W.apPw?[h("code",{},apName()),chip("New password","ok")]:[h("code",{},apName()),chip("Password kept","")];
  const kv=h("dl",{class:"qs-kv"},
    h("dt",{},"Access point"),h("dd",{},...apRow),
    h("dt",{},"Home WiFi"),h("dd",{},h("code",{},W.ssid),chip(W.wifiAuth&&W.wifiAuth!=="OPEN"?(W.wifiAuth.includes("WPA3")?"WPA2/3":"WPA2"):"protected",""),h("span",{},"mode Access point + Station")),
    h("dt",{},W.use==="mqtt"?"MQTT broker":"Home Assistant"),h("dd",{},...useRow),
    h("dt",{},"Vehicle data"),h("dd",{},chip("After the restart",""),h("span",{},"profile, protocol and standard PIDs come next")));
  return screen("Review, then restart","Everything below is saved together in one restart. Nothing has been written to WiCAN yet.",
    [kv,err,sub("What happens next"),
     steps([["WiCAN saves and restarts","About 15 seconds."],
       [W.apPw?"Your phone drops off "+apName():"Your phone may drop off "+apName()+" for a moment",W.apPw?"The access point comes back with the new password.":"The access point comes back with the same password."],
       ["Connect your phone to "+W.ssid+" and open the link on the next screen","Quick Setup continues there."]])],
    [back("wifi"),grow(),save]);
};

async function applyAll(saveBtn,errEl){
  saveBtn.disabled=true;errEl.replaceChildren();
  const staged=[];
  try{
    if(!D.wifi)D.wifi=strip(await api("/api/settings/wifi_manager"));
    const wf=strip(D.wifi);
    wf.mode="apsta";wf.sta_ssid=W.ssid;wf.sta_trusted=true;
    if(W.wifiPw)wf.sta_password=W.wifiPw;
    if(W.apPw)wf.ap_password=W.apPw;
    store.stage("wifi_manager",wf);staged.push("wifi_manager");
    if(W.use==="mqtt"){
      const mv=strip(D.mqtt||await api("/api/settings/mqtt_manager"));
      mv.enabled=true;mv.url=W.mqtt.url.trim();mv.username=W.mqtt.user||"";mv.topic_prefix=(W.mqtt.prefix||"").trim();
      if(W.mqtt.pw)mv.broker_password=W.mqtt.pw;
      store.stage("mqtt_manager",mv);staged.push("mqtt_manager");
      const dd=strip(D.dest||await api("/api/settings/data_destinations"));
      dd.enabled=true;
      const list=(dd.destinations||[]).map(d=>({...d}));
      let r=list.find(d=>d.type==="mqtt"&&String(d.url||"").replace(/\s/g,"")==="~/autopid");
      if(r){r.enabled=true;r.period_s=W.mqtt.period;}
      else{
        if(list.length>=8)throw new Error("The data destinations table is full (8 rows): remove one under Automate > Data destinations first");
        const names=new Set(list.map(d=>d.name));let nm="autopid",k=2;while(names.has(nm))nm="autopid"+(k++);
        list.push({name:nm,type:"mqtt",enabled:true,url:"~/autopid",period_s:W.mqtt.period,auth:"none",auth_token:"",auth_name:"",basic_username:"",basic_password:"",api_key:"",query:"",cert_set:"",car_model:"",retain:true,full_first:true});}
      dd.destinations=list;
      store.stage("data_destinations",dd);staged.push("data_destinations");
    }
    W.applied=true;W.cur="reconnect";
    try{history.replaceState(null,"","#/setup/reconnect");}catch(_){}
    paint();
    await store.commit("Quick Setup: saving and restarting");
  }catch(e){
    staged.forEach(c=>store.unstage(c));
    W.cur="review";try{history.replaceState(null,"","#/setup/review");}catch(_){}
    paint();
    /* the review screen is rebuilt: put the message where the user looks.
       A network-level failure (the AP drops its client for a few seconds
       around a scan) is not a refusal: say so and invite a second press */
    const lost=(e instanceof TypeError)||/failed to fetch|networkerror|load failed/i.test(e.message||"");
    setTimeout(()=>{const slot=scrEl.querySelector(".qs-body > div:empty");
      const b=lost?banner("warn","alert",h("b",{},"Lost contact with WiCAN for a moment. "),"Nothing was changed. Press Save and restart again.")
                  :banner("crit","alert",h("b",{},"WiCAN did not accept the settings: "),e.message||String(e));
      if(slot)slot.replaceWith(b);else scrEl.querySelector(".qs-body").prepend(b);},50);
  }
}

SCREENS.reconnect=()=>{
  const ssid=W.ssid||(D.wifi&&D.wifi.sta_ssid)||"your home WiFi";
  const link="http://"+mdnsHost()+"/#/setup/checks";
  const live=h("div",{});
  const paintLive=async()=>{
    if(conn.state!=="online"){live.replaceChildren(check("run",null,"Restarting","Waiting for WiCAN, up to 90 s. This page keeps trying."));return;}
    const w=await tryGet("/api/wifi/status");
    if(!w){live.replaceChildren(check("run",null,"Reconnecting","Waiting for WiCAN."));return;}
    if(w.sta_connected&&w.ip){
      live.replaceChildren(check("ok","wifi","WiCAN joined "+ssid,["Address ",h("code",{},w.ip),". If the link above does not open, use ",h("a",{href:"http://"+w.ip+"/#/setup/checks"},"http://"+w.ip+"/#/setup/checks"),"."],chip("Connected","ok")));
      return;}
    const at=w.sta_attempt||{};
    if(at.fail_count>0||(at.reason&&at.reason!==0)){
      live.replaceChildren(check("warn","alert","WiCAN could not join "+(at.ssid||ssid)+" yet",
        ["Last attempt failed (reason "+at.reason+", "+at.fail_count+" tries). A wrong password is the usual cause. "],
        btn("Fix WiFi",()=>go("wifi"),"sm")));return;}
    live.replaceChildren(check("run",null,"Joining "+ssid,"Usually a few seconds."));
  };
  paintLive();every(3000,paintLive);
  const copyBtn=btn("Copy link",()=>copyText(link,copyBtn),"",{icon:"copy"});
  return screen("Now switch networks",["Connect this phone or PC to ",h("b",{},ssid),", then open the link. Quick Setup picks up where it left off."],
    [h("div",{class:"qs-link"},
       h("div",{class:"rowflex",style:"flex-wrap:wrap"},chip("Step 1: join "+ssid+" on this device","info"),chip("Step 2: open","info")),
       h("span",{class:"url"},link),
       h("div",{class:"acts"},h("a",{class:"btn pri",href:link},ic("send"),"Open WiCAN"),copyBtn)),
     live,
     h("details",{class:"qs-details"},h("summary",{},"The link does not open?"),
       steps([["Give it a moment","WiCAN needs a few seconds to join. Some Android phones cannot open .local names at all."],
         ["Use the access point instead",["Join ",h("code",{},apName())," with your new password and open ",h("code",{},"http://"+((D.wifi&&D.wifi.ap_ip)||"192.168.0.10")+"/#/setup/checks"),". That page shows the address WiCAN received on "+ssid+", and whether the join worked."]],
         ["Wrong WiFi password?","The same page says so and lets you fix it. WiCAN keeps its access point on, so you are never locked out."]]))],
    [h("span",{class:"ghost"},"Stay on this screen until you have switched networks.")]);
};

SCREENS.checks=async()=>{
  const cards=h("div",{class:"qs-body"});
  if(!D.mqtt)D.mqtt=strip(await tryGet("/api/settings/mqtt_manager"));
  const mqttOn=!!(D.mqtt&&D.mqtt.enabled)||W.use==="mqtt";
  const haOn=W.use==="ha"||(!mqttOn&&W.use!=="wifi")||W.use==="wifi"&&false;
  const refresh=async()=>{
    const[w,wh,ds]=await Promise.all([tryGet("/api/wifi/status"),haOn?tryGet("/api/webhook"):null,mqttOn?tryGet("/api/destinations"):null]);
    const b=(conn.status&&conn.status.bits)||{};
    const ssid=W.ssid||(w&&w.sta_attempt&&w.sta_attempt.ssid)||(D.wifi&&D.wifi.sta_ssid)||"your WiFi";
    const out=[];
    if(w&&w.sta_connected)out.push(check("ok","wifi","WiFi: joined "+ssid,["Address ",h("code",{},w.ip||""),". Also reachable as ",h("code",{},"http://"+mdnsHost()),"."],chip("Connected","ok")));
    else out.push(check("warn","wifi","WiFi: not connected"+(w&&w.sta_attempt&&w.sta_attempt.ssid?" to "+w.sta_attempt.ssid:""),
      w&&w.sta_attempt&&w.sta_attempt.fail_count?"Last attempt failed (reason "+w.sta_attempt.reason+"). Check the password.":"WiCAN is still trying, or the network is out of range.",btn("Fix WiFi",()=>go("wifi"),"sm")));
    if(w&&w.ap_default_password===false||(!w&&!apDefaultPassword()))out.push(check("ok","lock","Access point secured",[h("code",{},apName())," now uses your password. It stays on as a fallback way in."],chip("Done","ok")));
    else out.push(check("warn","lock","Access point still has the factory password","Set your own password so nobody else can join.",btn("Set password",()=>go("ap"),"sm")));
    if(haOn){
      const url=wh&&wh.url;
      if(url){const host=(()=>{try{return new URL(url).origin;}catch(_){return url;}})();
        const age=wh.last_post?Math.max(0,Math.round((Date.now()-Date.parse(wh.last_post))/1000)):null;
        const ok=wh.status==="ok"||(wh.success_count||0)>0;
        out.push(check(ok?"ok":"warn","home",ok?"Home Assistant is connected":"Home Assistant registered, first push pending",
          ["Registered from ",h("code",{},host),". ",age!=null&&ok?"Last push "+age+" s ago, ":"",(wh.success_count||0)+" pushes, "+(wh.fail_count||0)+" failed."+(wh.last_error?" Last error: "+wh.last_error:"")],chip(ok?"Connected":"Waiting",ok?"ok":"warn")));}
      else out.push(check("warn","home","Waiting for Home Assistant",["In Home Assistant open Settings > Devices & services. ",h("b",{},"Discovered: WiCAN "+(deviceId()||""))," should be there. Press Add. This card turns green within a minute. If it does not appear, press Add Integration, search WiCAN and enter ",h("code",{},mdnsHost()),"."],chip("Waiting","warn")));
    }
    if(mqttOn){
      const url=(D.mqtt&&D.mqtt.url)||W.mqtt.url;
      if(b.mqtt_connected)out.push(check("ok","cloud","MQTT broker connected",[h("code",{},url)," as "+((D.mqtt&&D.mqtt.username)||W.mqtt.user||"anonymous")+". Status topic published; vehicle data starts after the next step."+(ds&&ds.destinations?" "+(ds.destinations.filter(d=>d.enabled).length)+" destination(s) active.":"")],chip("Connected","ok")));
      else out.push(check("crit","cloud","Could not connect to the broker",[h("code",{},url||"(no address)"),": not connected. Check the address and port, and that the broker allows this user. "],btn("Fix broker settings",()=>go("details"),"sm")));
    }
    cards.replaceChildren(...out);
  };
  await refresh();every(3000,refresh);
  return screen("Connected through "+(W.ssid||(D.wifi&&D.wifi.sta_ssid)||"your WiFi"),"You are now reaching WiCAN over your home WiFi. Quick checks before the vehicle part.",
    [cards],
    [btn("Finish without vehicle data",()=>{W.noVeh=true;go("done");},"gh sm"),grow(),btn("Set up my vehicle",()=>go("vehicle"),"pri",{icon:"car"})]);
};

/* ---- the vehicle step, second pass (2026-10-01): detect, identify, scan, THEN profile ----
   The device keeps a store of cars (GET /api/autopid/vehicles, keyed by VIN
   or by the set of answering ECUs). Detection is one job (the std scan with
   phases); a known car comes back with its profile, an unknown one gets
   standard PIDs only until a profile is chosen here. Nothing is saved
   until Finish. */
const WMI={KMH:"Hyundai",KM8:"Hyundai",KMF:"Hyundai",KNA:"Kia",KND:"Kia",KNE:"Kia",KNC:"Kia",
  WVW:"Volkswagen",WV1:"Volkswagen",WV2:"Volkswagen",WVG:"Volkswagen","3VW":"Volkswagen","1VW":"Volkswagen","9BW":"Volkswagen",
  WAU:"Audi",WA1:"Audi",TRU:"Audi",WUA:"Audi",WBA:"BMW",WBS:"BMW",WBX:"BMW",WBY:"BMW","5UX":"BMW","4US":"BMW",
  WP0:"Porsche",WP1:"Porsche",LGX:"BYD",LC0:"BYD",SAJ:"Jaguar",SAD:"Jaguar",JN1:"Nissan",JN8:"Nissan","1N4":"Nissan",SJN:"Nissan",VSK:"Nissan",
  W0L:"Opel",W0V:"Opel",VF1:"Renault",VF2:"Renault",JTM:"Toyota",JTD:"Toyota",JTN:"Toyota",JTE:"Toyota",JTH:"Lexus",SB1:"Toyota","2T3":"Toyota","4T1":"Toyota","5TD":"Toyota",
  "5YJ":"Tesla",LRW:"Tesla",XP7:"Tesla","7SA":"Tesla",TMB:"Skoda",VSS:"Seat",YV1:"Volvo",LYV:"Volvo",LPS:"Polestar",
  WDD:"Mercedes",WDC:"Mercedes",W1K:"Mercedes",W1N:"Mercedes",SAL:"Land Rover",ZFA:"Fiat",VR3:"Peugeot",VF3:"Peugeot",VR7:"Citroen",VF7:"Citroen",
  LSJ:"MG",SDP:"MG",JHM:"Honda","1HG":"Honda","2HG":"Honda","19X":"Honda",SHH:"Honda",WF0:"Ford","1FA":"Ford","1FT":"Ford","3FA":"Ford",
  "1G1":"Chevrolet","1GC":"Chevrolet",KL1:"Chevrolet","1C4":"Jeep","2C3":"Chrysler",JM1:"Mazda",JM3:"Mazda",JA3:"Mitsubishi",JA4:"Mitsubishi",JF1:"Subaru",JF2:"Subaru"};
const MAKE_ALIAS={volkswagen:["vw"],mercedes:["mercedes-benz","mb"],"land rover":["landrover","range rover"]};
const wmiMake=vin=>{const v=String(vin||"").toUpperCase();return v.length>=3?(WMI[v.slice(0,3)]||""):"";};
const sameMake=(a,b)=>{a=String(a||"").toLowerCase().trim();b=String(b||"").toLowerCase().trim();if(!a||!b)return false;
  if(a===b||a.startsWith(b)||b.startsWith(a))return true;
  const al=(MAKE_ALIAS[a]||[]).concat(MAKE_ALIAS[b]||[]);return al.includes(a)||al.includes(b);};
const defaultName=v=>{const mk=wmiMake(v.vin);const tail=v.vin?v.vin.slice(-4):(v.fingerprint||"").slice(0,4);
  return mk?mk+" ("+tail+")":(v.vin?"Vehicle ("+tail+")":(v.fingerprint?"Vehicle "+tail:""));};
const fmtAge=ts=>{if(!ts)return "";const d=Math.round(Date.now()/1000)-ts;if(d<0||d>1e9)return "";
  if(d<120)return "just now";if(d<7200)return Math.round(d/60)+" min ago";if(d<172800)return Math.round(d/3600)+" h ago";return Math.round(d/86400)+" days ago";};

SCREENS.vehicle=async()=>{
  if(!D.autopid){D.autopid=strip(await tryGet("/api/settings/autopid"));D.autopidSchema=await tryGet("/api/settings/autopid/schema");}
  D.vehicles=await tryGet("/api/autopid/vehicles"); /* null = firmware without the store */
  const maxName=(D.autopidSchema&&D.autopidSchema.properties&&D.autopidSchema.properties.vehicle&&D.autopidSchema.properties.vehicle.maxLength)||63;
  const err=h("div",{});
  const body=h("div",{class:"qs-body"});
  const finish=btn("Continue",()=>go("power"),"pri");
  const picker=vehicleProfilePicker({current:()=>W.profCar?W.profCar.car_model:(W.veh&&W.veh.profile)||"",onPick:car=>{W.profCar=car;W.profChoice="car";render();}});
  const entries=()=>(D.vehicles&&Array.isArray(D.vehicles.vehicles))?D.vehicles.vehicles:[];
  const protoName=p=>PROTO[String(p)]||(String(p)==="0"||!p?"Automatic":"protocol "+p);
  const render=()=>{
    const out=[];
    const v=W.veh;
    const phase=W.vehPhase;
    if(phase==="idle"||phase==="noanswer"){
      out.push(sub("1. Detect the vehicle"));
      if(phase==="noanswer")out.push(banner("warn","alert",h("b",{},"The car did not answer. "),
        (W.scanStatus&&W.scanStatus.error?W.scanStatus.error+". ":"")+"Usually the ignition is off or the plug is not seated. Turn the ignition on (an EV in ready mode) and try again. You can also skip: WiCAN detects the car by itself the first time it answers, and the Status page tells you when it has."));
      const detBtn=btn(phase==="noanswer"?"Try again":"Detect my vehicle",startDetect,"pri",{icon:"refresh",disabled:!(W.plugged&&W.ignition)});
      const ack=(key,id,title,text)=>h("label",{class:"qs-agree"},h("input",{type:"checkbox",id,checked:W[key],onchange:e=>{W[key]=e.target.checked;detBtn.disabled=!(W.plugged&&W.ignition);}}),h("div",{},h("b",{},title),h("span",{},text)));
      out.push(ack("plugged","qs-plugged","WiCAN is plugged into the OBD port","Under the dashboard, driver side on most cars."));
      out.push(ack("ignition","qs-ignition","Ignition is on","The engine can stay off. On an EV, \"ready\" mode."));
      out.push(h("div",{},detBtn));
      out.push(note("","About 15 to 30 s. WiCAN learns the protocol, reads the VIN and finds the standard PIDs, then remembers all of it under this vehicle. Plug it into another car later and it tells the cars apart."));
      if(!D.vehicles)out.push(note("","This firmware keeps one vehicle only (no vehicle store): the result is stored, but moving WiCAN between cars needs a new scan each time."));
    }else if(phase==="detecting"){
      out.push(sub("1. Detecting the vehicle"));
      const phases=[["protocol","Detecting the OBD protocol","WiCAN asks on each CAN protocol in turn, first the OBD-II way (0100), then the way of newer vans and trucks (ISO 27145: 22 F400). A few seconds."],["vin","Reading the VIN","Mode 09 PID 02, then UDS 22 F190 (22 F802 on an ISO 27145 vehicle). Without either, the car is identified by the set of ECUs that answered."],["pids","Finding the standard PIDs","The support bitmaps of every ECU that answers (0100, 0120 and up, or 22 F400 and up). Stored under this vehicle."],["network","Listening to the vehicle network","Is this a J1939 network (trucks, buses, agricultural machines)? WiCAN only listens here: the controllers that broadcast, the groups they send, the VIN when one is broadcast."]];
      const ph=W.scanStatus&&W.scanStatus.phase;const idx=Math.max(0,phases.findIndex(p=>p[0]===ph));
      phases.forEach((p,i)=>out.push(check(i<idx?"ok":i===idx?"run":"","info",p[1],p[2],i<idx?chip("Done","ok"):i===idx?chip("Working",""):null)));
    }else{
      /* result */
      out.push(sub("1. Vehicle detected"));
      const known=!!(v.known&&v.profile);
      const j1939Only=v.dialect==="j1939";
      if(known)out.push(banner("ok","check",h("b",{},"Welcome back: "+(v.name||defaultName(v))+". "),"WiCAN set this car up before and already switched to its profile, PIDs and protocol. Nothing to do here unless you want to change something."));
      else if(j1939Only)out.push(banner("ok","check",h("b",{},"J1939 vehicle. "),"This vehicle speaks SAE J1939, the network of trucks, buses and agricultural machines. Its controllers broadcast their values; WiCAN listens and never asks. "+(v.std_supported||0)+" standard parameters were heard and stored under this vehicle."+(v.vin?"":" No VIN was broadcast: it is remembered by the set of controllers on its network.")));
      else if(!v.vin)out.push(banner("info","info",h("b",{},"New vehicle. "),"This car gave no VIN, so WiCAN remembers it by the set of ECUs that answered (that works for one car; two identical models would look the same). Give it a name below."));
      else out.push(banner("ok","check",h("b",{},v.known?"Vehicle recognised. ":"New vehicle. "),"WiCAN stored the protocol, VIN and "+(v.std_supported||0)+" standard PIDs under this car. Pick a profile below so it can read the vehicle-specific values too."));
      if(v.j1939&&!v.j1939Listening)out.push(banner("warn","alert",h("b",{},"One more restart for the J1939 listener. "),"The J1939 network is read by WiCAN's own CAN controller, which is off on a new device. Finish turns it on in listen-only mode (it never transmits) together with the J1939 listener; the values arrive after that restart."));
      else if(v.j1939&&!j1939Only)out.push(banner("info","info",h("b",{},"A J1939 network as well. "),"This vehicle answers OBD requests and broadcasts J1939 groups. Both sets of parameters are stored under it."));
      const nameInp=h("input",{id:"qs-veh-name",value:W.vehName,placeholder:"e.g. Family car",style:"max-width:260px",oninput:e=>{W.vehName=e.target.value;}});
      out.push(h("dl",{class:"qs-kv"},
        h("dt",{},"VIN"),h("dd",{},v.vin?[h("code",{},v.vin),chip(j1939Only?"Broadcast by the vehicle":"Read from the car","ok")]:[chip("VIN not available","warn"),h("span",{},(j1939Only?"identified by its controllers":"identified by its ECUs")+(v.fingerprint?" ("+v.fingerprint+")":""))]),
        h("dt",{},j1939Only?"Network":"OBD protocol"),h("dd",{},j1939Only?[h("span",{id:"qs-veh-dialect"},"SAE J1939"+(v.busKbps?", "+v.busKbps+" kbit/s":"")),chip("Heard","ok")]:[protoName(v.protocol),DIALECT[v.dialect]?h("span",{id:"qs-veh-dialect"},DIALECT[v.dialect]):null,PROTO[String(v.protocol)]?chip("Detected","ok"):chip("Automatic",""),v.j1939?chip("J1939 network","ok"):null]),
        h("dt",{},j1939Only?"Standard parameters":"Standard PIDs"),h("dd",{},(v.std_supported||0)+(j1939Only?" heard":" supported"),chip("Stored","ok")),
        h("dt",{},"Name"),h("dd",{},nameInp,h("span",{class:"help",style:"font-size:12px"},"shown on the Status page and in Home Assistant"))));
      /* 2. profile (a J1939-only vehicle has no OBD requests for a profile to add) */
      if(j1939Only&&!known){
        if(W.profChoice===null){W.profChoice="none";W.profCar=null;}
        out.push(sub("2. Vehicle profile"));
        out.push(note("","Profiles carry OBD requests, which a J1939 vehicle does not answer. The standard J1939 parameters are stored already; more groups can be added as PGN rows under Automate > Parameters."));
      }
      const chosen=W.profChoice==="car"?W.profCar:null;
      if(!(j1939Only&&!known)){
      out.push(sub("2. Vehicle profile"));
      const testBtn=btn("Test profile",()=>testProfile(chosen||(known?{car_model:v.profile,fromStore:true}:null)),"sm",{icon:"refresh",disabled:!(chosen||known)});
      if(known&&W.profChoice===null){
        out.push(h("div",{class:"qs-pick"},h("span",{class:"sel"},ic("car"),v.profile),btn("Change profile",()=>picker.open(),"sm",{icon:"search"}),testBtn,btn("Scan again",startDetect,"sm",{icon:"refresh"})));
        out.push(note("","The profile and the PIDs you had for this car are back in place. Scan again only if the car behaves differently than before."));
      }else{
        const mk=wmiMake(v.vin);
        const sug=(W.profList||[]).filter(c=>mk&&sameMake(picker.splitName(c.car_model).make,mk)).slice(0,6);
        out.push(h("p",{class:"qs-lead",style:"font-size:13px"},
          mk&&sug.length?["The VIN says ",h("b",{},mk)," (manufacturer code "+v.vin.slice(0,3)+"). Profiles for "+mk+":"]
          :mk?["The VIN says ",h("b",{},mk),", but the published list has no "+mk+" profile yet. Search all profiles, or keep the standard PIDs for now."]
          :W.profList===null?"The published profile list could not be fetched (this phone needs internet). You can still keep the standard PIDs for now, or drop a profile file on Automate > Parameters later."
          :v.vin?["The manufacturer code ",h("code",{},v.vin.slice(0,3))," is not one WiCAN knows: search all profiles, or keep the standard PIDs for now."]
          :"No VIN to go on: search the published profiles, or keep the standard PIDs for now."));
        const radios=h("div",{class:"qs-radios"});
        sug.forEach(c=>radios.append(h("label",{},h("input",{type:"radio",name:"qs-prof",checked:chosen===c,onchange:()=>{W.profCar=c;W.profChoice="car";render();}})," "+c.car_model)));
        radios.append(h("label",{},h("input",{type:"radio",name:"qs-prof",id:"qs-prof-other",checked:chosen&&!sug.includes(chosen),onchange:()=>picker.open()})," Another profile: ",btn("Choose profile",()=>picker.open(),"sm",{icon:"search"}),chosen&&!sug.includes(chosen)?h("span",{class:"sel",style:"padding:3px 9px;margin-left:6px"},chosen.car_model):null));
        radios.append(h("label",{},h("input",{type:"radio",name:"qs-prof",id:"qs-prof-none",checked:W.profChoice==="none",onchange:()=>{W.profChoice="none";W.profCar=null;render();}})," Keep it without a profile (standard PIDs only). You can add one later from Automate > Parameters."));
        out.push(radios);
        out.push(h("div",{class:"qs-pick",style:"margin-top:6px"},testBtn,h("span",{class:"help",style:"font-size:12.5px"},chosen?"Sends each of the profile's requests to the car once and shows what comes back, before anything is saved.":"Pick a profile to test it against the car.")));
        if(chosen){const pm=profileProtocol(chosen);if(pm&&PROTO[String(v.protocol)]&&pm!==String(v.protocol))out.push(banner("warn","alert",h("b",{},"Protocol mismatch. "),"This profile expects "+protoName(pm)+" but the car answered on "+protoName(v.protocol)+". It may still work for some requests; test it first."));}
        out.push(note("","A profile adds the values the standard PIDs do not carry (battery state of charge, charging power). It is saved under this car: WiCAN applies it again whenever this car answers, and never to another car."));
      }
      }
    }
    /* the store */
    const list=entries();
    if(D.vehicles){
      out.push(sub("Vehicles this WiCAN knows"));
      if(list.length){
        out.push(h("div",{class:"qs-nets"},...list.map(e=>h("div",{class:"qs-net",style:"grid-template-columns:1fr auto auto;cursor:default"},
          h("span",{},h("b",{},e.name||defaultName(e)||"Unnamed vehicle"),h("br"),h("span",{class:"help"},(e.vin||("no VIN, "+(e.dialect==="j1939"?"controller set ":"ECU set ")+(e.fingerprint||"")))+" · "+(e.dialect==="j1939"?"SAE J1939":protoName(e.protocol)+(DIALECT[e.dialect]?", "+DIALECT[e.dialect]:"")+(e.j1939?", J1939 network":""))+" · "+(e.profile||"no profile")+" · "+(e.std_supported||0)+(e.dialect==="j1939"?" std parameters":" std PIDs")+(e.last_seen?" · seen "+fmtAge(e.last_seen):""))),
          e.current?chip("Current","pri"):e.pending_profile?chip("Profile pending","warn"):h("span",{}),
          btn("Forget",async()=>{if(!await confirmModal("Forget "+(e.name||e.vin||"this vehicle")+" and its PIDs?","Forget vehicle"))return;
            try{await api("/api/autopid/vehicles/"+encodeURIComponent(e.key),{method:"DELETE"});toast("Forgotten","ok");D.vehicles=await tryGet("/api/autopid/vehicles");render();}catch(ex){toast(ex.message,"err");}},"sm gh")))));
      }else out.push(h("div",{class:"empty"},"No vehicle stored yet. Detect one above."));
      out.push(note("","Up to "+(D.vehicles.max||8)+" vehicles. The current one is whichever answered last; its PIDs, protocol and profile are what WiCAN polls. Forget removes a car and its PIDs."));
    }
    out.push(err);
    body.replaceChildren(...out);
    finish.disabled=!(phase==="result"&&(W.profChoice!==null||(v&&v.known&&v.profile)));
  };
  /* the published list, for the suggestions (null = not reachable) */
  async function loadProfiles(){if(W.profList===undefined){W.profList=await picker.load();}}
  async function startDetect(){
    try{
      const st0=await tryGet("/api/autopid/std_scan");
      if(st0&&st0.status==="running")toast("A scan is already running","");
      else{
        try{await api("/api/autopid/vehicles/detect",{method:"POST",body:{}});}
        catch(e){if(/404|not found/i.test(e.message))await api("/api/autopid/std_scan",{method:"POST",body:{}});else throw e;}
      }
      W.vehPhase="detecting";W.veh=null;W.profChoice=null;W.profCar=null;
      W.scanStatus=(await tryGet("/api/autopid/std_scan"))||{status:"running"};render();
      loadProfiles();
      let ticks=0;
      const t=every(1200,async()=>{
        const s=await tryGet("/api/autopid/std_scan");W.scanStatus=s;
        if(s&&s.status==="running"&&ticks++<150){render();return;}
        clearInterval(t);
        const r=await tryGet("/api/autopid/std_scan/result");
        D.vehicles=await tryGet("/api/autopid/vehicles");
        if(!s||s.status==="failed"||!r||(!(r.supported||[]).length&&!r.vin&&!r.protocol_detected&&!r.j1939)){W.vehPhase="noanswer";render();return;}
        const list=entries();
        const key=r.key||r.vin||(r.fingerprint?"fp:"+r.fingerprint:"");
        const e=list.find(x=>x.key===key)||list.find(x=>x.current)||{};
        W.veh={key:e.key||key,vin:r.vin||e.vin||"",fingerprint:r.fingerprint||e.fingerprint||"",protocol:r.protocol_detected||e.protocol||r.protocol||"",dialect:r.dialect||e.dialect||"obd2",
          std_supported:(r.supported||[]).length||e.std_supported||0,known:!!(r.known||e.profile),name:r.name||e.name||"",profile:e.profile||"",specific_init:e.specific_init||"",pending_profile:e.pending_profile,
          j1939:!!(r.j1939||e.j1939),j1939Listening:r.j1939_listening!==false,busKbps:r.bus_kbps||0};
        W.scanResult=r;W.vehName=W.veh.name||defaultName(W.veh);
        await loadProfiles();
        W.vehPhase="result";render();
      });
    }catch(e){W.vehPhase="noanswer";W.scanStatus={error:e.message};render();}
  }
  /* Test profile: one real request per profile PID through POST /api/autopid/test,
     every parameter decoded from that reply; shown in a popup, nothing saved */
  async function testProfile(car){
    if(!car)return;
    let rows;
    if(car.fromStore){const list=(W.profList||await picker.load())||[];const c=list.find(x=>x.car_model===car.car_model);if(!c){toast("The published list does not have this profile any more: test it from Automate > Parameters","");return;}car=c;}
    rows=profileToPids(car,[]).pids;
    const tb=h("tbody",{});
    const summary=chip("Testing…","");
    const body=h("div",{},
      h("p",{class:"help",style:"margin:0 0 10px;font-size:12.5px"},"Each of the profile's requests is sent to the car once (its init chain, then the request); the values are decoded from those replies with the profile's expressions. A row with no answer means the car did not reply to that request: the parameter will stay empty if you use this profile."),
      h("div",{class:"scroll"},h("table",{class:"tbl"},h("thead",{},h("tr",{},h("th",{},"Parameter"),h("th",{class:"num"},"Value"),h("th",{},"Unit"),h("th",{},"Request"))),tb)));
    const close=modal({title:"Test: "+car.car_model,body:h("div",{},h("div",{class:"rowflex",style:"margin-bottom:8px"},summary,h("span",{class:"grow"})),body),
      actions:[{label:"Close"},{label:"Use this profile",kind:"pri",fn:()=>{W.profCar=car;W.profChoice="car";render();}}]});
    const m=document.querySelector("#modal-root .modal");if(m)m.classList.add("wide");
    let ok=0,total=0,stop=false;
    const prev=document.getElementById("modal-root").onclick;document.getElementById("modal-root").onclick=e=>{if(e.target===e.currentTarget){stop=true;prev&&prev(e);}};
    for(const row of rows){
      if(stop||!tb.isConnected)break;
      const prm=row.parameters||[];
      let res=null,errMsg="";
      for(let attempt=0;attempt<2&&!res;attempt++){
        try{res=await api("/api/autopid/test",{method:"POST",body:{cmd:row.cmd,init:row.init||"",type:"specific",expressions:prm.map(p=>p.expression)}});}
        catch(e){errMsg=e.message;if(/409|busy|another/i.test(e.message)){await new Promise(r=>setTimeout(r,1200));}else break;}
      }
      prm.forEach((p,i)=>{
        total++;
        const val=res&&res.ok&&Array.isArray(res.values)?res.values[i]:null;
        const good=val!==null&&val!==undefined&&!Number.isNaN(val);
        if(good)ok++;
        tb.append(h("tr",{style:good?"":"color:var(--text-3)"},
          h("td",{},h("b",{},p.name),p.name!==row.name?h("span",{class:"help"}," "+row.name):null),
          h("td",{class:"num mono",style:good?"":"color:var(--danger)"},good?String(Number.isInteger(val)?val:Math.round(val*100)/100):(res&&res.ok?"null":"no answer")),
          h("td",{},p.unit||""),
          h("td",{class:"mono",style:"font-size:11px;color:var(--text-3)"},row.cmd+(res&&res.raw?": "+String(res.raw).replace(/\s+/g," ").slice(0,60):errMsg?": "+errMsg.slice(0,60):""))));
      });
      summary.replaceChildren(h("span",{class:"d"}),ok+" of "+total+" parameters answered");
    }
    summary.className="chip "+(total&&ok===total?"ok":ok?"warn":"crit");
    summary.replaceChildren(h("span",{class:"d"}),ok+" of "+total+" parameters answered"+(stop?" (stopped)":""));
    void close;
  }
  if(W.vehPhase===undefined||W.vehPhase===null)W.vehPhase="idle";
  if(W.vehPhase==="result")await loadProfiles();
  render();
  return screen("Your vehicle","WiCAN detects the car first: the OBD protocol, the VIN and the standard PIDs it supports. Then you pick a profile for the vehicle-specific values. Everything is stored under the car, so moving WiCAN between cars just works.",
    [body],
    [back("checks"),grow(),btn("Skip for now",()=>{W.noVeh=true;go("done");},"gh sm"),finish]);
};
/* the protocol a profile's init chains name (ATSP/ATTP n), "" when none */
function profileProtocol(car){
  const rx=/AT\s*[ST]P\s*([0-9A-C])/i;
  let m=rx.exec(car.init||"");if(m)return m[1].toUpperCase();
  for(const p of(car.pids||[])){m=rx.exec(p.pid_init||"");if(m)return m[1].toUpperCase();}
  return "";
}

/* ---- step 10: battery and sleep (2026-10-01, Ali) ----
   Two readings from the car make the Power Saving pair: the charging voltage
   with the engine running and the resting voltage after key-off. The wake
   voltage is a setting of its own since sleep_manager v3 (wake_mv). Every
   voltage shows ONE decimal (Ali). The capture machine (pwrStep) and the
   rule (pwrRecommend) are pure; the screen polls GET /api/battery once a
   second (the sampler refreshes every poll_s from an 8-sample burst). */
const r1=x=>Math.round(x*10+1e-6)/10;
const fmtV=v=>(v==null||!isFinite(v))?"--.-":Number(v).toFixed(1);
const pwrLast=(p,k)=>p.trace.slice(-k).map(x=>x.v);
const mean=a=>a.reduce((x,y)=>x+y,0)/a.length;
const spread=a=>Math.max(...a)-Math.min(...a);
/* one reading in; returns true when the phase changed */
function pwrStep(p,v,now){
  if(v==null||!isFinite(v)){p.misses=(p.misses||0)+1;if(p.misses>=5&&!p.trace.length&&p.phase==="running"){p.phase="nobatt";return true;}return false;}
  p.misses=0;
  p.trace.push({t:now,v});if(p.trace.length>90)p.trace.shift();
  if(p.base==null&&p.trace.length>=3)p.base=mean(pwrLast(p,3));
  if(p.phase==="nobatt"){if(v>=11){p.phase="running";p.trace=[{t:now,v}];p.base=null;return true;}return false;}
  if(p.phase==="running"){
    if(p.trace.length>=3&&mean(pwrLast(p,3))<9){p.phase="nobatt";return true;}
    if(p.trace.length>=6){const a=pwrLast(p,6);
      /* charging = stable and clearly above a resting battery; a smart alternator
         charges lower: a climb of 0.4 V from the first readings, then 12 s stable */
      const lowCharge=p.trace.length>=12&&p.base!=null&&spread(pwrLast(p,12))<0.1&&mean(a)>=p.base+0.4&&mean(a)>=12.6;
      if((spread(a)<0.15&&mean(a)>=13.3)||lowCharge){p.charging=mean(a);p.tC=now;p.phase="charged";return true;}}
    return false;
  }
  if(p.phase==="charged"){
    const a=pwrLast(p,2);
    if(a.length===2&&Math.max(...a)<p.charging-0.5){p.phase="dropping";p.tD=now;return true;}
    return false;
  }
  if(p.phase==="dropping"){
    const a=pwrLast(p,10);
    if(a.length===10&&((spread(a)<0.04&&mean(a)<p.charging-0.4)||now-p.tD>=60000)){p.resting=mean(pwrLast(p,5));p.tR=now;p.phase="rested";return true;}
    return false;
  }
  return false;
}
/* a little above resting for sleep, a little above sleep but well below
   charging for wake; narrow margins are flagged, never hidden */
function pwrRecommend(p,rng){
  const gap=p.charging-p.resting;
  let sleep=r1(p.resting+(gap<0.5?0.2:0.3));
  let wake=gap<0.5?r1(sleep+0.1):Math.min(r1(sleep+0.2),r1(p.charging-0.4));
  if(wake<sleep+0.1)wake=r1(sleep+0.1);
  sleep=Math.min(rng.sMax,Math.max(rng.sMin,sleep));wake=Math.min(rng.wMax,Math.max(rng.wMin,wake));
  if(wake<sleep+0.1)wake=r1(sleep+0.1);
  p.sleep=sleep;p.wake=wake;p.touched=false;p.narrow=pwrNarrow(p);
}
const pwrNarrow=p=>p.charging!=null&&p.resting!=null&&(r1(p.charging-p.wake)<=0.3||r1(p.sleep-p.resting)<0.2);
function pwrChip(p){
  if(p.phase==="nobatt")return chip("No battery","warn");
  if(p.phase==="running")return chip("Waiting for the engine","");
  if(p.phase==="charged")return chip("Charging","ok");
  if(p.phase==="dropping")return chip("Dropping","warn");
  return chip("Resting","ok");
}
/* the live trace: the last 90 readings, grid at whole volts, the two
   thresholds dashed with labels, the captured readings marked */
function pwrSvg(p){
  const W=600,H=150,L=34,R=92,T=12,B=138;
  const usb=p.phase==="nobatt";const lo=usb?0:11.5,hi=15;
  const y=v=>B-(Math.min(hi,Math.max(lo,v))-lo)/(hi-lo)*(B-T);
  const n=90,x=i=>L+(i/(n-1))*(W-L-R),off=n-p.trace.length;
  let s='<svg viewBox="0 0 '+W+' '+H+'" role="img" aria-label="Battery voltage, last 90 seconds">';
  for(const g of(usb?[0,5,10,15]:[12,13,14,15]))s+='<line class="g" x1="'+L+'" x2="'+(W-R)+'" y1="'+y(g).toFixed(1)+'" y2="'+y(g).toFixed(1)+'"/><text x="'+(L-6)+'" y="'+(y(g)+3.5).toFixed(1)+'" text-anchor="end">'+g+' V</text>';
  if(p.trace.length>1){const pts=p.trace.map((r,i)=>x(off+i).toFixed(1)+","+y(r.v).toFixed(1));
    s+='<polygon class="f" points="'+x(off).toFixed(1)+','+B+' '+pts.join(" ")+' '+x(n-1).toFixed(1)+','+B+'"/><polyline class="l" points="'+pts.join(" ")+'"/>';}
  const mark=(t,v)=>{const i=p.trace.findIndex(r=>r.t===t);return i>=0?'<circle class="m" cx="'+x(off+i).toFixed(1)+'" cy="'+y(v).toFixed(1)+'" r="4"/>':"";};
  if(p.charging!=null&&p.phase!=="running")s+=mark(p.tC,p.charging);
  if(p.phase==="rested"){s+=mark(p.tR,p.resting);
    for(const [k,v] of [["sleep",p.sleep],["wake",p.wake]]){const yy=y(v).toFixed(1);
      s+='<line class="t '+k+'" x1="'+L+'" x2="'+(W-R)+'" y1="'+yy+'" y2="'+yy+'"/><text class="'+k+'" x="'+(W-R+6)+'" y="'+(Number(yy)+(k==="sleep"?10:-3)).toFixed(1)+'">'+k+' '+fmtV(v)+' V</text>';}}
  if(p.trace.length){const r=p.trace[p.trace.length-1];s+='<circle class="m e" cx="'+x(n-1).toFixed(1)+'" cy="'+y(r.v).toFixed(1)+'" r="3.5"/>';}
  return s+'</svg>';
}
function pwrWarn(p){
  if(p.phase!=="rested")return null;
  /* a slider dragged into a mistake is named first; the car's own small margins after */
  if(p.touched&&p.wake>p.charging-0.2)return banner("warn","alert",h("b",{},"Wake is above your charging voltage. "),"WiCAN would never wake up while you drive. Keep it at least 0.3 V under "+fmtV(p.charging)+" V.");
  if(p.touched&&p.sleep<=p.resting)return banner("warn","alert",h("b",{},"Sleep is below your resting voltage. "),"Parked, the battery stays above "+fmtV(p.sleep)+" V and WiCAN would never sleep.");
  if(p.narrow)return banner("warn","alert",h("b",{},"Small margins on this car. "),"It charges at only "+fmtV(p.charging)+" V, close to its resting "+fmtV(p.resting)+" V; a smart alternator does this once the battery is full. WiCAN can still tell parked from driving with these values, but keep the sleep delay at 5 minutes or more. If WiCAN ever falls asleep while you drive, lower both voltages a little; if it never sleeps, raise the sleep voltage.");
  return null;
}
SCREENS.power=async()=>{
  if(D.sleep===undefined)D.sleep=await tryGet("/api/settings/sleep_manager");
  if(D.sleepSchema===undefined)D.sleepSchema=await tryGet("/api/settings/sleep_manager/schema");
  const props=(D.sleepSchema&&D.sleepSchema.properties)||{};
  const lim=(k,dmin,dmax)=>{const q=props[k]||{};return [q.minimum!=null?q.minimum/1000:dmin,q.maximum!=null?q.maximum/1000:dmax];};
  const [sMin,sMax]=lim("sleep_mv",12,14),[wMin,wMax]=lim("wake_mv",12.1,15);
  const rng={sMin,sMax,wMin,wMax};
  const hasWake=!!props.wake_mv; /* older firmware derives wake = sleep + 0.1 V */
  const devSleep=D.sleep&&D.sleep.sleep_mv>0?D.sleep.sleep_mv/1000:13.1;
  const devWake=D.sleep&&D.sleep.wake_mv>0?D.sleep.wake_mv/1000:r1(devSleep+0.1);
  const delayMin=(D.sleep&&D.sleep.sleep_delay_min>0)?D.sleep.sleep_delay_min:5;
  const holdTxt=((D.sleep&&D.sleep.wake_delay_ms>0?D.sleep.wake_delay_ms:500)/1000).toFixed(1).replace(/\.0$/,"")+" s";
  const p=W.pwr;
  if(p.phase==="idle"){p.phase="running";p.trace=[];p.base=null;p.charging=null;p.resting=null;p.sleep=devSleep;p.wake=devWake;p.touched=false;p.narrow=false;}
  const body=h("div",{class:"qs-body"});
  let bigEl,chipEl,traceEl,sleepVal,wakeVal,sleepIn,wakeIn,sumEl,warnEl,recoEl;
  const summary=()=>["WiCAN sleeps once the battery has stayed below ",h("b",{},fmtV(p.sleep)+" V")," for "+delayMin+" minute"+(delayMin===1?"":"s")+" and wakes as soon as it rises above ",h("b",{},fmtV(p.wake)+" V"),". Margins on your car: "+fmtV(p.sleep-p.resting)+" V above resting, "+fmtV(p.charging-p.wake)+" V below charging."];
  const slide=(k,val)=>{
    p.touched=true;
    if(k==="sleep"){p.sleep=r1(val);if(p.wake<p.sleep+0.1)p.wake=r1(p.sleep+0.1);}
    else p.wake=Math.max(r1(val),r1(p.sleep+0.1));
    p.narrow=pwrNarrow(p);paintLive();
    if(recoEl&&!recoEl.querySelector("button"))recoEl.replaceChildren(btn("Back to the recommendation",()=>{pwrRecommend(p,rng);render();},"sm"));
  };
  const paintLive=()=>{
    const v=p.trace.length?p.trace[p.trace.length-1].v:null;
    if(bigEl)bigEl.replaceChildren(fmtV(v),h("small",{},"V"));
    if(chipEl)chipEl.replaceChildren(pwrChip(p));
    if(traceEl)traceEl.innerHTML=pwrSvg(p);
    if(sleepVal)sleepVal.textContent=fmtV(p.sleep)+" V";
    if(wakeVal)wakeVal.textContent=fmtV(p.wake)+" V";
    if(sleepIn&&Number(sleepIn.value)!==p.sleep)sleepIn.value=p.sleep;
    if(wakeIn&&Number(wakeIn.value)!==p.wake)wakeIn.value=p.wake;
    if(sumEl&&p.phase==="rested")sumEl.replaceChildren(...summary());
    if(warnEl){const w=pwrWarn(p);warnEl.replaceChildren(w||"");}
  };
  const useNow=()=>{
    if(p.trace.length<3)return;
    const now=Date.now();
    if(p.phase==="running"){p.charging=mean(pwrLast(p,3));p.tC=now;p.phase="charged";}
    else if(p.phase==="charged"){p.phase="dropping";p.tD=now;}
    else if(p.phase==="dropping"){p.resting=mean(pwrLast(p,3));p.tR=now;p.phase="rested";pwrRecommend(p,rng);}
    render();
  };
  const right=(...els)=>h("div",{style:"display:flex;gap:8px;align-items:center;flex-wrap:wrap;justify-content:flex-end"},...els);
  const useBtn=()=>{const b=btn("Use the current reading",useNow,"sm");b.id="qs-pwr-use";return b;};
  const render=()=>{
    const out=[];
    const v=p.trace.length?p.trace[p.trace.length-1].v:null;
    bigEl=h("div",{class:"big",id:"qs-pwr-big"},fmtV(v),h("small",{},"V"));
    chipEl=h("div",{class:"st",id:"qs-pwr-state"},pwrChip(p));
    traceEl=h("div",{class:"qs-trace",id:"qs-pwr-trace"});traceEl.innerHTML=pwrSvg(p);
    out.push(h("div",{class:"qs-live"},h("div",{},h("div",{class:"lbl"},"Battery now"),bigEl,chipEl,h("div",{class:"help",style:"font-size:11.5px;margin-top:10px;color:var(--text-3)"},"One reading a second, the last 90 s shown.")),traceEl));
    if(p.phase==="nobatt"){
      out.push(banner("info","info",h("b",{},"WiCAN is not seeing a car battery"),v==null?" (no reading yet)":" ("+fmtV(v)+" V)",". It is probably powered over USB on a desk. Keep the current values for now (sleep below "+fmtV(devSleep)+" V, wake above "+fmtV(devWake)+" V) and run Quick Setup again once it is in the car: this step takes about a minute there."));
    }else{
      const stA=p.phase==="running"?"run":"ok";
      const stB=p.phase==="running"?"":p.phase==="rested"?"ok":"run";
      out.push(sub("1. Engine running"));
      out.push(check(stA,"check","Start the engine",["Or put an EV or hybrid in Ready. WiCAN waits for the charging voltage to settle; most cars charge the 12 V battery at 13.8 to 14.8 V. If yours charges lower (a smart alternator), press ",h("strong",{},"Use the current reading")," once the engine runs."],
        stA==="ok"?chip(fmtV(p.charging)+" V charging","ok"):right(chip(p.base!=null&&v!=null&&v>p.base+0.3?"Settling":"Waiting for the engine",""),useBtn())));
      out.push(sub("2. Engine off"));
      const rowB=check(stB,"info","Turn the engine off and wait about half a minute","Leave the doors closed and everything switched off. The voltage drops at once, then settles as the battery rests. WiCAN notices the drop by itself.",
        stB==="ok"?chip(fmtV(p.resting)+" V resting","ok"):stB==="run"?right(chip(p.phase==="dropping"?"Dropping":"Waiting for the engine to stop",""),useBtn()):h("span",{}));
      if(stB==="")rowB.style.opacity=".55";
      out.push(rowB);
      if(p.phase==="rested"){
        out.push(sub("3. The two voltages"));
        warnEl=h("div",{id:"qs-pwr-warn"});const w=pwrWarn(p);if(w)warnEl.append(w);out.push(warnEl);
        sleepVal=h("span",{class:"val",id:"qs-pwr-sleep-val"},fmtV(p.sleep)+" V");
        sleepIn=h("input",{type:"range",id:"qs-pwr-sleep",min:sMin,max:sMax,step:0.1,value:p.sleep,"aria-label":"Sleep voltage",oninput:e=>slide("sleep",Number(e.target.value))});
        wakeVal=h("span",{class:"val",id:"qs-pwr-wake-val"},fmtV(p.wake)+" V");
        wakeIn=h("input",{type:"range",id:"qs-pwr-wake",min:wMin,max:wMax,step:0.1,value:p.wake,disabled:!hasWake,"aria-label":"Wake voltage",oninput:e=>slide("wake",Number(e.target.value))});
        out.push(h("div",{class:"qs-thr"},
          h("div",{class:"qs-thr-row"},h("b",{},h("i",{style:"background:var(--warning)"}),"Sleep below"),sleepVal,sleepIn,
            h("span",{},"Parked, the battery rests at about ",h("b",{},fmtV(p.resting)+" V")," on your car. Once it has stayed below this voltage for "+delayMin+" minute"+(delayMin===1?"":"s")+", WiCAN sleeps.")),
          h("div",{class:"qs-thr-row"},h("b",{},h("i",{style:"background:var(--success)"}),"Wake above"),wakeVal,wakeIn,
            h("span",{},hasWake?["Driving, the engine charges at about ",h("b",{},fmtV(p.charging)+" V"),". Above this voltage for "+holdTxt+", WiCAN wakes up and starts polling again."]:"This firmware derives the wake voltage: 0.1 V above sleep."))));
        sumEl=h("p",{class:"qs-lead",id:"qs-pwr-sum",style:"font-size:13px"},...summary());out.push(sumEl);
        recoEl=h("div",{id:"qs-pwr-reco",style:"display:flex;gap:8px;align-items:center;flex-wrap:wrap"},
          p.touched?btn("Back to the recommendation",()=>{pwrRecommend(p,rng);render();},"sm"):h("span",{class:"help",style:"font-size:12.5px"},"Recommended from your two readings: a little above resting for sleep, well below charging for wake. Drag either slider to change it."));
        out.push(recoEl);
        out.push(note("","Both voltages and the "+delayMin+" minute delay live under Power Saving afterwards. Power Saving is switched on when you continue. WiCAN only reads the voltage; it never switches anything in the car off."));
      }else{
        out.push(note("","Why measure: with the wrong voltages WiCAN either never sleeps and slowly drains the 12 V battery, or falls asleep while you drive and your data stops. One minute here settles both."));
      }
    }
    body.replaceChildren(...out);
  };
  const tick=async()=>{
    const b=await tryGet("/api/battery");
    const v=b&&typeof b.voltage==="number"?b.voltage:null;
    if(!body.isConnected)return;
    if(pwrStep(p,v,Date.now())){if(p.phase==="rested")pwrRecommend(p,rng);render();}
    else paintLive();
  };
  render();
  await tick();
  every(1000,tick);
  const keepLabel=p.phase==="nobatt"?"Continue with the current values":"Keep the current values ("+fmtV(devSleep)+" / "+fmtV(devWake)+" V)";
  const keep=btn(keepLabel,()=>{p.skipped=true;p.measured=false;go("polling");},p.phase==="nobatt"?"pri":"gh sm");keep.id="qs-pwr-keep";
  const cont=p.phase==="nobatt"?null:btn("Continue",()=>{p.measured=true;p.skipped=false;go("polling");},"pri",{disabled:p.phase!=="rested"});if(cont)cont.id="qs-pwr-continue";
  /* the footer follows the phase (Continue unlocks once both readings are in) */
  const refreshFoot=()=>{if(cont)cont.disabled=p.phase!=="rested";};
  every(1000,refreshFoot);
  return screen("When should WiCAN sleep?","WiCAN watches your car's 12 V battery. With the engine running the battery is being charged and its voltage is high; parked, it rests lower. WiCAN sleeps once the voltage has stayed below the sleep voltage for "+delayMin+" minute"+(delayMin===1?"":"s")+", and wakes when it rises above the wake voltage. Measure both on your car in about a minute.",
    [body],[back("vehicle"),grow(),keep,cont]);
};

/* ---- step 11: how WiCAN reads the car (the autopid polling rules, 2026-10-01) ----
   Plain-language versions of the Automate settings that decide how much data
   the user gets and what happens when the car is parked. Prefilled from the
   device; staged with the vehicle step's Finish (one restart). The poll rate
   is the default group's period in the LIVE config; the rest is settings. */
SCREENS.polling=async()=>{
  if(!D.autopid){D.autopid=strip(await tryGet("/api/settings/autopid"));D.autopidSchema=await tryGet("/api/settings/autopid/schema");}
  if(D.sleep===undefined)D.sleep=await tryGet("/api/settings/sleep_manager");
  if(D.cfg==null)D.cfg=await tryGet("/api/autopid/config");
  const maxName=(D.autopidSchema&&D.autopidSchema.properties&&D.autopidSchema.properties.vehicle&&D.autopidSchema.properties.vehicle.maxLength)||63;
  const a=D.autopid||{};
  /* prefill once from the device */
  if(!W.pollSeeded){
    const g=(D.cfg&&Array.isArray(D.cfg.groups)&&D.cfg.groups.length)?D.cfg.groups[0]:null;
    const sec=g&&g.period_ms?Math.round(g.period_ms/1000):0;
    const p=W.poll;
    /* a device that already polls keeps its rate; a fresh one (polling off,
       the firmware's 1 s default group) gets the recommended 5 s */
    if(a.enabled){if([1,2,5,10].includes(sec))p.rate=sec;else if(sec>0){p.rate="custom";p.rateCustom=sec;}}
    if(a.min_event_interval_ms)p.minEvent=Math.max(0.1,a.min_event_interval_ms/1000);
    if(a.pause_below_mv>0){p.pause="custom";p.pauseV=a.pause_below_mv/1000;}
    else p.pause=a.pause_follow_sleep===false?"never":"sleep";
    p.pauseAll=a.pause_mode==="all";
    if(typeof a.std_enabled==="boolean")p.std=a.std_enabled;
    if(typeof a.custom_enabled==="boolean")p.custom=a.custom_enabled;
    if(typeof a.specific_enabled==="boolean")p.specific=a.specific_enabled;
    p.dtc=!!a.dtc_enabled;if(a.dtc_scan_period_min>0)p.dtcMin=a.dtc_scan_period_min;
    W.pollSeeded=true;
  }
  const p=W.poll;
  const devSleepV=D.sleep&&D.sleep.sleep_mv>0?D.sleep.sleep_mv/1000:13.1;
  const sleepV=fmtV(W.pwr.measured?W.pwr.sleep:devSleepV)+" V";
  const wakeV=fmtV(W.pwr.measured?W.pwr.wake:(D.sleep&&D.sleep.wake_mv>0?D.sleep.wake_mv/1000:r1(devSleepV+0.1)))+" V";
  const hasProfile=!!(W.profChoice==="car"&&W.profCar)||!!(W.veh&&W.veh.known&&W.veh.profile&&W.profChoice!=="none");
  const err=h("div",{});
  const body=h("div",{class:"qs-body"});
  const finish=btn("Finish and restart",()=>finishVehicle(finish,err,maxName),"pri",{icon:"power"});
  const render=()=>{
    const out=[];
    const radio=(name,id,checked,title,text,onpick,extra)=>h("label",{class:"qs-agree",style:"padding:10px 14px"},
      h("input",{type:"radio",name,id,checked,onchange:onpick}),h("div",{},h("b",{},title),h("span",{},...(Array.isArray(text)?text:[text])),extra||null));
    const sw=(id,label,desc,on,disabled,onchange,tail)=>h("div",{class:"qs-check",style:"grid-template-columns:minmax(0,1fr) auto"},
      h("div",{},h("b",{},label),h("span",{class:"d2"},desc),tail||null),
      h("label",{class:"switch",title:disabled?"Not available":""},h("input",{type:"checkbox",id,checked:on,disabled,onchange:e=>onchange(e.target.checked)}),h("span",{class:"sw"}),h("span",{class:"swl"},disabled?"n/a":on?"On":"Off")));
    out.push(sub("1. How often"));
    const rates=[[1,"Every second","For live driving data and dashboards. Keeps the car's modules busiest."],[2,"Every 2 seconds","A good middle ground when you watch values while driving."],[5,"Every 5 seconds (recommended)","Plenty for Home Assistant and MQTT dashboards, gentle on the car."],[10,"Every 10 seconds","Trip logging, charging state, long-term monitoring."]];
    const rateBox=h("div",{class:"qs-radios",style:"gap:8px"});
    rates.forEach(([v,t,d])=>rateBox.append(radio("qs-rate","qs-rate-"+v,p.rate===v,t,d,()=>{p.rate=v;render();})));
    const custIn=h("input",{type:"number",min:1,max:600,value:p.rateCustom,style:"width:80px;display:inline;padding:3px 6px",onchange:e=>{p.rateCustom=Math.max(1,Math.min(600,Number(e.target.value)||1));p.rate="custom";render();}});
    rateBox.append(radio("qs-rate","qs-rate-custom",p.rate==="custom","Custom",["Every ",custIn," seconds"],()=>{p.rate="custom";render();}));
    out.push(rateBox);
    out.push(note("","WiCAN asks the car for every parameter at this rate. Faster shows changes sooner but keeps the modules busier and the bus noisier. A profile can still give single values their own rate later, under Automate > Parameters."));
    const evIn=h("input",{id:"qs-minevent",type:"number",min:0.1,max:600,step:0.1,value:p.minEvent,style:"max-width:110px",onchange:e=>{p.minEvent=Math.max(0.1,Math.min(600,Number(e.target.value)||1));}});
    out.push(h("div",{class:"qs-row",style:"margin-top:4px"},h("label",{for:"qs-minevent"},"Report a change at most every"),h("div",{class:"ctl"},evIn,h("span",{},"seconds")),
      h("div",{class:"help"},"Limits how fast value changes are pushed to Home Assistant, MQTT and the rules. Slower than the poll rate it never does anything.")));
    out.push(sub("2. When the car is off"));
    const pauseBox=h("div",{class:"qs-radios",style:"gap:8px"});
    pauseBox.append(radio("qs-pause","qs-pause-sleep",p.pause==="sleep","Pause with Power Saving (recommended)","Requests stop when the 12 V battery drops below the Power Saving sleep voltage ("+sleepV+(W.pwr.measured?", measured on your car":"")+": engine off) and resume above the wake-up voltage ("+wakeV+"). The car's modules can sleep and the battery stays charged.",()=>{p.pause="sleep";render();}));
    const slider=h("input",{type:"range",min:12,max:14.5,step:0.1,value:p.pauseV,style:"width:260px;max-width:100%;vertical-align:middle",oninput:e=>{p.pauseV=Number(e.target.value);vLbl.textContent=p.pauseV.toFixed(1)+" V";},onchange:()=>{p.pause="custom";render();}});
    const vLbl=h("span",{class:"mono"},p.pauseV.toFixed(1)+" V");
    pauseBox.append(radio("qs-pause","qs-pause-custom",p.pause==="custom","Pause below a voltage I choose",["Stops below the voltage you set, resumes 0.3 V above it. ",h("br"),slider," ",vLbl],()=>{p.pause="custom";render();}));
    pauseBox.append(radio("qs-pause","qs-pause-never",p.pause==="never","Never pause","WiCAN keeps asking while the car is parked.",()=>{p.pause="never";render();}));
    out.push(pauseBox);
    if(p.pause==="never")out.push(banner("warn","alert",h("b",{},"This keeps the car's modules awake. "),"With the engine off for days it can drain the 12 V battery. Pick this only for a bench or a car on a charger."));
    out.push(sw("qs-pauseall","While paused, also stop listening","Off: the J1939 rows, which are read from WiCAN's own CAN listener and transmit nothing, keep running while requests are paused. On: they stop too and nothing is read at all.",p.pauseAll,false,v=>{p.pauseAll=v;render();}));
    out.push(sub("3. What to read"));
    const nStd=(W.veh&&W.veh.std_supported)||0;
    out.push(sw("qs-std","Standard PIDs","Speed, RPM, coolant, fuel level and the other values every car reports."+(nStd?" The scan found "+nStd+" of them on this car.":""),p.std,false,v=>{p.std=v;render();}));
    out.push(sw("qs-specific","Vehicle-specific values",hasProfile?"Battery state of charge, charging power and the other values your profile knows how to ask for.":"No profile chosen for this car yet. Pick one on the previous step, or later under Automate > Parameters.",hasProfile&&p.specific,!hasProfile,v=>{p.specific=v;render();}));
    out.push(sw("qs-custom","Custom PIDs","Requests you add yourself under Automate > Parameters.",p.custom,false,v=>{p.custom=v;render();}));
    out.push(sub("4. Trouble codes"));
    const dtcIn=h("input",{id:"qs-dtcmin",type:"number",min:5,max:10080,value:p.dtcMin,style:"width:90px;display:inline;padding:3px 6px",onchange:e=>{p.dtcMin=Math.max(5,Math.min(10080,Number(e.target.value)||60));}});
    out.push(sw("qs-dtc","Check for trouble codes","Reads the stored fault codes (the check-engine light) and shows them under Trouble Codes and in Home Assistant. Clearing codes stays a manual action, off unless you allow it there.",p.dtc,false,v=>{p.dtc=v;render();},
      p.dtc?h("div",{style:"margin-top:8px;font-size:13px"},"Every ",dtcIn," minutes while the car is on"):null));
    out.push(err);
    body.replaceChildren(...out);
  };
  render();
  return screen("How WiCAN reads the car","These rules decide how much data you get and what happens when the car is parked. The defaults suit Home Assistant and MQTT; change them if you know what you want. Everything here can be changed later under Automate.",
    [body],[back("power"),grow(),finish]);
};

async function finishVehicle(finishBtn,errEl,maxName){
  finishBtn.disabled=true;errEl.replaceChildren();
  try{
    const v=W.veh||{};
    const car=W.profChoice==="car"?W.profCar:null;
    const none=W.profChoice==="none";
    const p=W.poll;
    const rateS=p.rate==="custom"?p.rateCustom:p.rate;
    /* 1. the car's entry: name + profile choice (the store applies the init live) */
    if(D.vehicles&&v.key){
      const bodyPut={name:W.vehName||defaultName(v)};
      if(car){bodyPut.profile=String(car.car_model||"").slice(0,maxName);bodyPut.specific_init=car.init||"";}
      else if(none){bodyPut.profile="";bodyPut.specific_init="";}
      await api("/api/autopid/vehicles/"+encodeURIComponent(v.key),{method:"PUT",body:bodyPut});
    }
    /* 2. the PID tables: the profile's rows replace the vehicle-specific ones; std + custom
       stay; the default group takes the chosen rate and the rows that sat on the old
       default inherit it */
    const cfg0=await tryGet("/api/autopid/config");
    const cfg=cfg0&&typeof cfg0==="object"?JSON.parse(JSON.stringify(cfg0)):{};
    cfg.pids=Array.isArray(cfg.pids)?cfg.pids:[];
    cfg.groups=Array.isArray(cfg.groups)&&cfg.groups.length?cfg.groups:[{name:"default",enabled_default:true,period_ms:1000}];
    cfg.filters=Array.isArray(cfg.filters)?cfg.filters:[];
    const oldPeriod=cfg.groups[0].period_ms||1000;
    if(car||none){
      const keep=cfg.pids.filter(pd=>pd.type!=="specific");
      if(car){const r=profileToPids(car,keep);cfg.pids=keep.concat(r.pids);if(r.renamed.length)toast(r.renamed.length+" duplicate parameter name"+(r.renamed.length>1?"s":"")+" renamed","");}
      else cfg.pids=keep;
    }
    /* a firmware without the store never scanned by itself: add the found standard rows */
    if(!D.vehicles&&W.scanResult&&W.scanResult.supported){
      const norm=c=>String(c||"").replace(/\s+/g,"").toUpperCase();const have=new Set(cfg.pids.map(x=>norm(x.cmd)));
      for(const rw of W.scanResult.supported){if(have.has(norm(rw.cmd)))continue;have.add(norm(rw.cmd));
        cfg.pids.push({name:rw.name,cmd:rw.cmd,group:"default",period_ms:0,type:"std",parameters:(rw.parameters||[]).map(x=>({...x}))});}
    }
    cfg.groups[0].period_ms=Math.max(1,rateS)*1000;
    for(const pd of cfg.pids){if(pd.period_ms===oldPeriod||pd.period_ms===1000)pd.period_ms=0;}
    await api("/api/autopid/config",{method:"PUT",body:cfg});
    /* 3. the settings: polling on, protocol follows the store, the reading rules, the profile name for the backup */
    const av=strip(D.autopid||await api("/api/settings/autopid"));
    av.enabled=true;av.std_protocol="0";
    av.std_enabled=!!p.std;av.custom_enabled=!!p.custom;
    if(car){av.vehicle=String(car.car_model||"").slice(0,maxName);av.specific_init=car.init||"";av.specific_enabled=!!p.specific;}
    else if(none){av.vehicle="";av.specific_init="";av.specific_enabled=false;}
    else if(v.known&&v.profile){av.specific_enabled=!!p.specific;}
    av.min_event_interval_ms=Math.round(Math.max(0.1,p.minEvent)*1000);
    av.pause_below_mv=p.pause==="custom"?Math.round(p.pauseV*1000):0;
    av.pause_follow_sleep=p.pause==="sleep";
    av.pause_mode=p.pauseAll?"all":"requests_only";
    av.dtc_enabled=!!p.dtc;
    av.dtc_scan_period_min=p.dtc?Math.max(5,p.dtcMin):0;
    store.stage("autopid",av);
    /* 3b. a J1939 network heard with the listener off: the native CAN controller
       listen-only (it never transmits) at the bitrate the detection measured, and
       the J1939 listener, in the same restart */
    if(v.j1939&&!v.j1939Listening){
      const cm=strip(await api("/api/settings/can_manager"));
      cm.enabled=true;cm.silent=true;
      const kb=String(v.busKbps||"");
      cm.baud=(cm.baud==="auto"||!kb)?"auto":kb;
      store.stage("can_manager",cm);
      const jl=strip(await api("/api/settings/j1939"));
      jl.enabled=true;
      store.stage("j1939",jl);
    }
    /* 4. the Battery and sleep step: the measured pair switches Power Saving on (wake_mv only
       when this firmware has it; an older one derives wake = sleep + 0.1 V) */
    if(W.pwr.measured){
      const sm=strip(D.sleep||await api("/api/settings/sleep_manager"));
      sm.enabled=true;sm.sleep_mv=Math.round(W.pwr.sleep*1000);
      if(D.sleepSchema&&D.sleepSchema.properties&&D.sleepSchema.properties.wake_mv)sm.wake_mv=Math.round(W.pwr.wake*1000);
      store.stage("sleep_manager",sm);
    }
    W.noVeh=false;W.cur="done";
    try{history.replaceState(null,"","#/setup/done");}catch(_){}
    paint();
    await store.commit("Quick Setup: applying vehicle settings");
  }catch(e){
    store.unstage("autopid");store.unstage("sleep_manager");store.unstage("can_manager");store.unstage("j1939");finishBtn.disabled=false;
    errEl.replaceChildren(banner("crit","alert",h("b",{},"WiCAN did not accept the vehicle setup: "),e.message||String(e)));
  }
}

SCREENS.done=async()=>{
  const body=h("div",{class:"qs-body"});
  const refresh=async()=>{
    const out=[];
    if(conn.state!=="online"){out.push(check("run",null,"Restarting","WiCAN applies the vehicle settings. About 15 s; this page waits."));}
    const[w,ap,vs]=await Promise.all([tryGet("/api/wifi/status"),tryGet("/api/settings/autopid"),tryGet("/api/autopid/vehicles")]);
    const v=vs&&Array.isArray(vs.vehicles)?(vs.vehicles.find(x=>x.current)||null):null;
    const b=(conn.status&&conn.status.bits)||{};
    const mqttOn=!!(D.mqtt&&D.mqtt.enabled)||W.use==="mqtt";
    const kv=[];
    kv.push(h("dt",{},"Reach WiCAN at"),h("dd",{},h("code",{},"http://"+mdnsHost()),w&&w.ip?[h("span",{},"or"),h("code",{},"http://"+w.ip)]:null));
    kv.push(h("dt",{},"Access point"),h("dd",{},h("code",{},apName()),chip(w&&w.ap_default_password===false?"Your password":"Check the password",w&&w.ap_default_password===false?"ok":"warn")));
    if(mqttOn)kv.push(h("dt",{},"MQTT broker"),h("dd",{},chip(b.mqtt_connected?"Connected":"Not connected",b.mqtt_connected?"ok":"crit"),h("code",{},(D.mqtt&&D.mqtt.url)||W.mqtt.url||"")));
    else if(W.use!=="wifi"){const wh=await tryGet("/api/webhook");kv.push(h("dt",{},"Home Assistant"),h("dd",{},chip(wh&&wh.url?"Connected":"Waiting for discovery",wh&&wh.url?"ok":"warn")));}
    const vehOn=ap&&ap.enabled;
    kv.push(h("dt",{},"Vehicle"),h("dd",{},!vehOn?chip("Not set up",""):[v&&v.name?h("b",{},v.name):null,(v&&v.profile)||ap.vehicle?h("code",{},(v&&v.profile)||ap.vehicle):chip("Standard PIDs only",""),
      v&&v.vin?h("span",{},"VIN ",h("code",{},v.vin)):null,v&&v.dialect==="j1939"?h("span",{},"SAE J1939, listening"):v&&PROTO[String(v.protocol)]?h("span",{},PROTO[String(v.protocol)]+(DIALECT[v.dialect]?", "+DIALECT[v.dialect]:"")+(v.j1939?", J1939 network":"")):(ap.std_protocol==="0"?chip("Protocol: follows the car",""):null),
      v&&v.pending_profile?chip("Profile pending","warn"):null]));
    if(vehOn&&!W.noVeh){const pp=W.poll;const rate=pp.rate==="custom"?pp.rateCustom:pp.rate;
      const pauseTxt=pp.pause==="sleep"?"pauses with Power Saving":pp.pause==="custom"?"pauses below "+pp.pauseV.toFixed(1)+" V":"never pauses";
      kv.push(h("dt",{},"Reading the car"),h("dd",{},h("span",{},"every "+rate+" s, "+pauseTxt+(pp.dtc?", trouble codes every "+pp.dtcMin+" min":""))));}
    if(vehOn&&!W.noVeh&&W.pwr.measured){const dm=(D.sleep&&D.sleep.sleep_delay_min>0)?D.sleep.sleep_delay_min:5;
      kv.push(h("dt",{},"Battery and sleep"),h("dd",{},h("span",{},"sleeps after "+dm+" min below ",h("b",{},fmtV(W.pwr.sleep)+" V"),", wakes above ",h("b",{},fmtV(W.pwr.wake)+" V")),chip("Measured on your car","ok")));}
    out.push(h("dl",{class:"qs-kv"},...kv));
    if(W.pwr.skipped&&!W.noVeh){const sl=await tryGet("/api/sleep");
      out.push(note("",["Power Saving keeps its current values"+(sl&&sl.sleep_v?" (sleep below "+fmtV(sl.sleep_v)+" V, wake above "+fmtV(sl.wake_v)+" V)":"")+". If WiCAN sleeps while you drive or never sleeps when parked, run Quick Setup again and measure your car on the Battery and sleep step."]));}
    if(conn.sleepEnabled===false&&!W.pwr.measured)out.push(banner("warn","alert",h("b",{},"Power saving is off. "),"WiCAN stays awake and can drain the vehicle battery when the car is parked for days. Turn on sleep under Power Saving when you are happy with the setup."));
    out.push(sub("Where to go next"));
    out.push(h("div",{class:"qs-next"},
      h("a",{href:"#/dashboard"},h("b",{},"Dashboard"),h("span",{},"Live values, one tile per parameter.")),
      h("a",{href:"#/automate/parameters"},h("b",{},"Automate > Parameters"),h("span",{},"The PID lists, groups and poll rates.")),
      h("a",{href:"#/settings/wifi"},h("b",{},"Settings"),h("span",{},"WiFi, access point, Bluetooth, MQTT.")),
      h("a",{href:"#/power"},h("b",{},"Power Saving"),h("span",{},"Sleep when the car is off."))));
    out.push(note("","Run Quick Setup again any time: it is the first entry in the sidebar, and under System > Maintenance."));
    body.replaceChildren(...out);
  };
  await refresh();every(3000,refresh);
  return screen("WiCAN is set up",W.noVeh?"WiCAN is on your WiFi and secured. Vehicle data can be added any time from Automate or by running Quick Setup again.":"AutoPID is on. Values appear on the Dashboard as soon as the car is on.",
    [body],[grow(),btn("Open the Dashboard",()=>{location.hash="#/dashboard";},"pri")]);
};

/* ---------- the page ---------- */
PAGES.__setup=async(view,subId)=>{
  if(!document.getElementById("qs-css"))document.head.append(h("style",{id:"qs-css"},CSS));
  const p=page(view,null,"Quick Setup","Gets a new WiCAN onto your home WiFi, talking to Home Assistant or your MQTT broker, reading your vehicle and sleeping when the car is parked. About six minutes.",
    [h("button",{class:"btn sm gh",type:"button",onclick:()=>{location.hash="#/status";}},"Exit setup")]);
  railEl=h("aside",{class:"qs-rail","aria-label":"Setup steps"});
  scrEl=h("section",{class:"qs-screen","aria-live":"polite"});
  p.append(h("div",{class:"qs"},railEl,scrEl));
  if(IDS.includes(subId))W.cur=subId;
  else if(!W.cur)W.cur="safety";
  if(!D.wifi)D.wifi=strip(await tryGet("/api/settings/wifi_manager"));
  paint();
  return ()=>{stopTimers();railEl=null;scrEl=null;};
};
})();
