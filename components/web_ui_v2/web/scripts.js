/* web_ui_v2 — the Scripts page as an on-demand chunk (2026-09-07).
   Loaded by index.html's PAGES.scripts stub from /ui/scripts.js (embedded,
   gzipped; web_ui_v2/CMakeLists.txt minifies it with the same rjsmin as the
   page) the first time the page opens. A classic script: it shares the
   page's global scope (h, page, tabbedPage, settingsForm, api, UI_RES, …)
   and registers PAGES.__scripts plus the editor library in UI_RES. The
   preview (tools/webui_preview/make_preview.py) inlines it after the app
   script so the jsdom probes run it. */
(()=>{
const CSS=`/* Scripts page (2026-09-07): list | editor + output; CodeMirror themed with the page variables */
.scr-grid{display:grid;grid-template-columns:190px minmax(0,1fr);gap:16px;align-items:start}
.scr-list{display:flex;flex-direction:column;gap:2px}
.scr-item{display:flex;align-items:center;gap:8px;padding:7px 10px;border-radius:8px;border:1px solid transparent;background:transparent;color:var(--text);font:inherit;font-size:13px;text-align:left;cursor:pointer;width:100%;font-family:var(--mono)}
.scr-item:hover{background:var(--surface-2)}.scr-item.on{background:var(--primary-tint);color:var(--primary);font-weight:700}
.scr-item .sz{margin-left:auto;font-size:11px;color:var(--text-3);font-weight:400;font-family:var(--ui)}
.scr-bar{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin-bottom:8px}
.scr-bar input.nm{width:170px;font-family:var(--mono)}.scr-bar .st{font-size:12px;color:var(--text-3);margin-left:auto}
.scr-ed{border:1px solid var(--border);border-radius:9px;overflow:hidden;background:var(--surface)}
.scr-ed textarea{display:block;width:100%;min-height:400px;padding:12px 14px;border:0;resize:vertical;background:transparent;color:var(--text);font-family:var(--mono);font-size:13px;line-height:1.5;tab-size:2;outline:none;box-sizing:border-box}
.scr-ed .CodeMirror{height:440px;font-family:var(--mono);font-size:13px;line-height:1.5;background:transparent;color:var(--text)}
.scr-ed .CodeMirror-gutters{background:var(--surface-2);border-color:var(--border)}.scr-ed .CodeMirror-linenumber{color:var(--text-3)}
.scr-ed .CodeMirror-cursor{border-left:1.5px solid var(--text)}.scr-ed .CodeMirror-selected,.scr-ed .CodeMirror-focused .CodeMirror-selected{background:var(--primary-tint)}
.scr-ed .CodeMirror-activeline-background{background:color-mix(in srgb,var(--primary) 6%,transparent)}
.scr-ed .cm-line-err{background:var(--danger-tint)}
.scr-ed .cm-keyword{color:var(--primary);font-weight:700}.scr-ed .cm-string{color:var(--success)}.scr-ed .cm-comment{color:var(--text-3);font-style:italic}
.scr-ed .cm-number,.scr-ed .cm-atom{color:var(--warning)}.scr-ed .cm-builtin{color:var(--accent)}.scr-ed .cm-variable-2{color:var(--text);font-weight:600}.scr-ed .cm-def{color:var(--accent);font-weight:700}
.scr-ed .CodeMirror-matchingbracket{outline:1px solid var(--primary);color:inherit!important}
.CodeMirror-hints{font-family:var(--mono);font-size:12.5px;background:var(--surface);border:1px solid var(--border);box-shadow:var(--shadow-lg);border-radius:8px;color:var(--text)}
.CodeMirror-hint{color:var(--text)}li.CodeMirror-hint-active{background:var(--primary);color:var(--on-primary)}
.scr-out{margin-top:10px;background:var(--term-bg);color:var(--term-text);border:1px solid var(--border);border-radius:9px;padding:10px 14px;font-family:var(--mono);font-size:12.5px;line-height:1.5;white-space:pre-wrap;word-break:break-word;min-height:54px;max-height:280px;overflow-y:auto}
.scr-out .er{color:var(--danger)}.scr-out a{color:var(--primary);cursor:pointer;text-decoration:underline}.scr-out .dim{color:var(--term-dim)}
.scr-hint{margin-top:6px;font-size:12.5px;color:var(--text-2)}
.ref-item{padding:9px 0;border-bottom:1px solid var(--border)}.ref-item:last-child{border-bottom:0}
.ref-sig{font-family:var(--mono);font-size:12.5px;color:var(--primary);font-weight:700}
.ref-doc{font-size:12.5px;color:var(--text-2);margin:3px 0}.ref-ex{font-family:var(--mono);font-size:12px;color:var(--text-3)}
.ref-item .btn{float:right;margin-left:8px}
.prim{display:grid;grid-template-columns:repeat(auto-fill,minmax(290px,1fr));gap:10px}
.prim>div{border:1px solid var(--border);border-radius:9px;padding:10px 12px}.prim b{display:block;font-size:12px;margin-bottom:4px}
.prim pre{margin:0;font-family:var(--mono);font-size:12px;line-height:1.5;white-space:pre-wrap;color:var(--text)}.prim .note{font-size:11.5px;color:var(--text-3);margin-top:4px}
.xcards{display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:12px}
.xcard{border:1px solid var(--border);border-radius:9px;padding:12px 14px;display:flex;flex-direction:column;gap:7px;background:var(--surface)}
.xcard h4{margin:0;font-size:14px}.xcard p{margin:0;font-size:12.5px;color:var(--text-2);flex:1;line-height:1.45}.xcard .rowflex{gap:6px;flex-wrap:wrap}`;
document.head.append(h("style",{},CSS));
UI_RES.cm={id:"cm",title:"code editor",label:"CodeMirror 5.65.18 (210 KB, MIT)",sizeTxt:"210 KB",need:450000,global:"CodeMirror",
    scripts:["codemirror.min.js","cm-simple.min.js","cm-matchbrackets.min.js","cm-closebrackets.min.js","cm-active-line.min.js","cm-show-hint.min.js","cm-comment.min.js"],
    css:["codemirror.min.css","cm-show-hint.min.css"],cdn:"https://cdn.jsdelivr.net/npm/codemirror@5.65.18/",
    files:[{name:"codemirror.min.js",path:"lib/codemirror.min.js",size:173953,fnv:0xcaaed239},{name:"codemirror.min.css",path:"lib/codemirror.min.css",size:6378,fnv:0x8937ca8c},
      {name:"cm-simple.min.js",path:"addon/mode/simple.min.js",size:4419,fnv:0x3231baed},{name:"cm-matchbrackets.min.js",path:"addon/edit/matchbrackets.min.js",size:3479,fnv:0x622b8f12},
      {name:"cm-closebrackets.min.js",path:"addon/edit/closebrackets.min.js",size:3918,fnv:0xf6b551fa},{name:"cm-active-line.min.js",path:"addon/selection/active-line.min.js",size:1700,fnv:0x0f022644},
      {name:"cm-show-hint.min.js",path:"addon/hint/show-hint.min.js",size:11055,fnv:0x3810d6d1},{name:"cm-show-hint.min.css",path:"addon/hint/show-hint.min.css",size:875,fnv:0x41e888dc},
      {name:"cm-comment.min.js",path:"addon/comment/comment.min.js",size:4469,fnv:0xa20d346f}],state:window.CodeMirror?"ready":"unknown",store:null};
/* ---- Scripts (2026-09-07): write, run and learn Berry scripts in the browser ----
   The firmware describes its own scripting API (GET /api/scripts/reference:
   bindings, globals, a Berry primer, error hints, limits) and ships the
   example gallery (GET /api/scripts/examples, ?id= for a source), so nothing here
   drifts from the bindings. Scripts are files under /data/scripts (saved
   through /api/fs/upload); Run sends the editor's text inline (or saves and
   runs by name past the inline cap); Check compiles without running.
   CodeMirror (highlighting, line numbers, autocomplete) is an on-demand
   library like the chart library: installed once into cache/www. */
const BERRY_KW="if elif else while for def end class break continue return true false nil var do import as try except raise static".split(" ");
const BERRY_BUILTIN="print str int real size format type classname list map range bytes assert string json math global".split(" ");
let berryModeReady=false;
function berryMode(){
  if(berryModeReady||!window.CodeMirror||!CodeMirror.defineSimpleMode)return;
  berryModeReady=true;
  const word=list=>new RegExp("(?:"+list.join("|")+")\\b");
  CodeMirror.defineSimpleMode("berry",{
    start:[
      {regex:/#-/,token:"comment",next:"block"},
      {regex:/#.*/,token:"comment"},
      {regex:/"(?:[^\\"]|\\.)*"?|'(?:[^\\']|\\.)*'?/,token:"string"},
      {regex:/0x[0-9a-fA-F]+|\d+(?:\.\d+)?(?:[eE][-+]?\d+)?/,token:"number"},
      {regex:/(def)(\s+)([A-Za-z_]\w*)/,token:["keyword",null,"def"]},
      {regex:word(BERRY_KW),token:"keyword"},
      {regex:word(BERRY_BUILTIN),token:"builtin"},
      {regex:/[A-Za-z_]\w*(?=\s*\()/,token:"variable-2"},
      {regex:/\.\.|[-+\/*=<>!&|^%~]+/,token:"operator"},
      {regex:/[A-Za-z_]\w*/,token:"variable"}
    ],
    block:[{regex:/.*?-#/,token:"comment",next:"start"},{regex:/.*/,token:"comment"}],
    meta:{lineComment:"#"}
  });
}
/* autocomplete: device bindings (with their signature), readable globals, keywords, builtins */
function berryHint(cm,ref){
  const cur=cm.getCursor(),tok=cm.getTokenAt(cur);
  const start=/\w/.test(tok.string)?tok.start:cur.ch,word=cm.getRange({line:cur.line,ch:start},cur);
  if(!/^\w+$/.test(word))return null;
  const binds=(ref&&ref.bindings)||[];
  const names=[...new Set([...binds.map(b=>b.name),...((ref&&ref.globals)||[]).map(g=>g.name).filter(n=>!n.includes("<")),...BERRY_KW,...BERRY_BUILTIN])];
  const list=names.filter(n=>n.startsWith(word)&&n!==word).map(n=>{const b=binds.find(x=>x.name===n);return b?{text:n+"(",displayText:b.sig}:{text:n};});
  return list.length?{list,from:{line:cur.line,ch:start},to:cur}:null;
}
/* one editor API over a plain textarea and, once installed, CodeMirror */
function makeEditor(host,opts){
  const ta=h("textarea",{spellcheck:"false",oninput:()=>opts.onChange&&opts.onChange()});
  host.replaceChildren(ta);
  const ed={cm:null,ta,errLine:null,
    get(){return ed.cm?ed.cm.getValue():ta.value;},
    set(v){if(ed.cm)ed.cm.setValue(v);else ta.value=v;ed.clearMark();},
    focus(){ed.cm?ed.cm.focus():ta.focus();},
    insert(txt){if(ed.cm){ed.cm.replaceSelection(txt);ed.cm.focus();}
      else{const a=ta.selectionStart,b=ta.selectionEnd;ta.value=ta.value.slice(0,a)+txt+ta.value.slice(b);ta.selectionStart=ta.selectionEnd=a+txt.length;ta.focus();}
      opts.onChange&&opts.onChange();},
    goto(line){ed.clearMark();
      if(ed.cm){ed.cm.setCursor({line:line-1,ch:0});ed.cm.addLineClass(line-1,"background","cm-line-err");ed.errLine=line-1;ed.cm.focus();}
      else{const ls=ta.value.split("\n");let pos=0;for(let i=0;i<line-1&&i<ls.length;i++)pos+=ls[i].length+1;ta.focus();ta.setSelectionRange(pos,pos+(ls[line-1]||"").length);}},
    clearMark(){if(ed.cm&&ed.errLine!=null){ed.cm.removeLineClass(ed.errLine,"background","cm-line-err");ed.errLine=null;}},
    upgrade(ref){
      if(ed.cm||!window.CodeMirror)return;
      berryMode();
      const hint=cm=>berryHint(cm,ref);
      ed.cm=CodeMirror.fromTextArea(ta,{mode:"berry",lineNumbers:true,matchBrackets:true,autoCloseBrackets:true,styleActiveLine:true,indentUnit:2,tabSize:2,indentWithTabs:false,lineWrapping:true,
        extraKeys:{"Ctrl-S":()=>opts.onSave&&opts.onSave(),"Cmd-S":()=>opts.onSave&&opts.onSave(),"Ctrl-Enter":()=>opts.onRun&&opts.onRun(),"Cmd-Enter":()=>opts.onRun&&opts.onRun(),
          "Ctrl-Space":"autocomplete","Ctrl-/":"toggleComment","Cmd-/":"toggleComment",Tab:cm=>{if(cm.somethingSelected())cm.indentSelection("add");else cm.replaceSelection("  ");},"Shift-Tab":"indentLess"},
        hintOptions:{hint,completeSingle:false}});
      ed.cm.on("change",()=>{ed.clearMark();opts.onChange&&opts.onChange();});
      ed.cm.on("inputRead",(cm,ch)=>{if(ch.origin==="+input"&&/\w/.test((ch.text||[""])[0])&&!cm.state.completionActive){const t=cm.getTokenAt(cm.getCursor());if(/^\w{2,}$/.test(t.string))cm.showHint({hint,completeSingle:false});}});
    }};
  ta.addEventListener("keydown",e=>{
    if(e.key==="Tab"){e.preventDefault();ed.insert("  ");}
    else if((e.ctrlKey||e.metaKey)&&e.key==="s"){e.preventDefault();opts.onSave&&opts.onSave();}
    else if((e.ctrlKey||e.metaKey)&&e.key==="Enter"){e.preventDefault();opts.onRun&&opts.onRun();}
  });
  return ed;
}
const SCR_DIR="/data/scripts";
const SCR_NEEDS={vehicle:"Needs a vehicle (or an ECU on the bus)",dtc:"Needs Trouble codes enabled",can:"Needs the CAN bus on"};
const SCR_LEVEL={1:"First steps",2:"Everyday",3:"Advanced"};
PAGES.__scripts=async(view,sub)=>{
  const p=page(view,null,"Scripts","Berry scripts on the WiCAN: write them here, run them by hand, or let a rule run them.");
  const wrap=h("div",{class:"pane"});p.append(wrap);
  const paneEd=h("div",{class:"pane"}),paneEx=h("div",{class:"pane"}),paneRef=h("div",{class:"pane"});
  tabbedPage(wrap,"scripts",[{id:"editor",el:paneEd},{id:"examples",el:paneEx},{id:"reference",el:paneRef}],sub);
  /* one request at a time with one retry: right after a restart the device
     answers slowly and a dropped request must not read as "scripting off" */
  const get2=async p=>(await tryGet(p))||(await new Promise(r=>setTimeout(r,900)),await tryGet(p));
  const st=await get2("/api/scripts"),ref=await get2("/api/scripts/reference"),exs=await get2("/api/scripts/examples");
  const examples=(exs&&exs.examples)||[],binds=(ref&&ref.bindings)||[],groups=(ref&&ref.groups)||[];
  const enabled=st?!!st.enabled:!!(ref&&ref.enabled);
  const fmtK=n=>n>=1024?(n/1024).toFixed(1)+" KB":n+" B";
  const cleanName=v=>{v=String(v||"").trim().replace(/\.be$/i,"");return /^[A-Za-z0-9_-]{1,36}$/.test(v)?v:null;};
  const exampleSrc=async id=>{const r=await fetch(API+"/api/scripts/examples?id="+encodeURIComponent(id),{cache:"no-store"});if(!r.ok)throw new Error("example "+r.status);return await r.text();};
  /* ---- editor state ---- */
  let cur=null,dirty=false,running=false,scripts=[];
  const listEl=h("div",{class:"scr-list"});
  const nameIn=h("input",{class:"nm",placeholder:"script_name",maxlength:36,oninput:()=>{dirty=true;paintStatus();}});
  const statusEl=h("span",{class:"st"});
  const edHost=h("div",{class:"scr-ed"});
  const outEl=h("div",{class:"scr-out"},h("span",{class:"dim"},"Output of Run and Check shows here."));
  const hintEl=h("div",{class:"scr-hint"});
  const ed=makeEditor(edHost,{onChange:()=>{if(!dirty){dirty=true;paintStatus();}},onSave:()=>save(),onRun:()=>run()});
  const btn=(label,icon,fn,cls,title)=>h("button",{class:"btn sm "+(cls||""),type:"button",title,onclick:fn},ic(icon),label);
  const runBtn=btn("Run","play",()=>run(),"pri","Run what is in the editor (Ctrl+Enter)"),stopBtn=btn("Stop","stop",()=>stop(),"","Stop the running script"),
    checkBtn=btn("Check","check",()=>check(),"","Compile without running: reports syntax errors"),saveBtn=btn("Save","save",()=>save(),"","Save to the WiCAN (Ctrl+S)"),
    dlBtn=btn("Download","download",()=>download(),"gh","Download the saved file"),delBtn=btn("Delete","trash",()=>del(),"gh danger","Delete the saved file");
  const insSel=h("select",{title:"Insert a call at the cursor",onchange:()=>{const b=binds.find(x=>x.name===insSel.value);if(b)ed.insert(b.ex+"\n");insSel.value="";}},
    h("option",{value:""},"Insert…"),...groups.map(g=>h("optgroup",{label:g.title},...binds.filter(b=>b.group===g.id).map(b=>h("option",{value:b.name},b.sig)))));
  const bar=h("div",{class:"scr-bar"},nameIn,h("span",{class:"dim",style:"margin-left:-2px"},".be"),saveBtn,runBtn,stopBtn,checkBtn,insSel,dlBtn,delBtn,statusEl);
  function paintStatus(){
    const canRun=enabled&&!running;
    runBtn.disabled=!canRun;checkBtn.disabled=!canRun;stopBtn.disabled=!running;
    runBtn.title=enabled?"Run what is in the editor (Ctrl+Enter)":"Scripting is off: turn it on below";
    dlBtn.disabled=!cur||dirty;delBtn.disabled=!cur;
    statusEl.textContent=running?"Running…":dirty?(cur?"Unsaved changes":"New script, not saved yet"):cur?"Saved":"";
    listEl.querySelectorAll(".scr-item").forEach(b=>b.classList.toggle("on",b.dataset.name===cur));
  }
  function paintList(){
    listEl.replaceChildren(...scripts.map(x=>h("button",{class:"scr-item"+(x.name===cur?" on":""),type:"button",dataset:{name:x.name},onclick:()=>open(x.name)},x.name.replace(/\.be$/,""),h("span",{class:"sz"},fmtK(x.size)))));
    if(!scripts.length)listEl.append(h("div",{class:"empty",style:"padding:14px 6px"},"No scripts yet. Start from an example, or click New."));
  }
  async function refreshList(){const r=await tryGet("/api/scripts");scripts=(r&&r.scripts)||[];paintList();paintStatus();}
  async function confirmDiscard(){return !dirty||await confirmModal("The editor has unsaved changes. Discard them?","Unsaved changes");}
  async function open(name){
    if(name!==cur&&!await confirmDiscard())return;
    try{const r=await fetch(API+"/api/fs/download?path="+encP(SCR_DIR+"/"+name),{cache:"no-store"});if(!r.ok)throw new Error("HTTP "+r.status);
      ed.set(await r.text());cur=name;nameIn.value=name.replace(/\.be$/,"");dirty=false;showOut("",true);paintStatus();}
    catch(e){toast("Could not open "+name+": "+e.message,"err");}
  }
  async function newScript(ex){
    if(!await confirmDiscard())return;
    const nIn=h("input",{class:"mono",value:ex?ex.id:"",placeholder:"my_script",maxlength:36});
    const sel=ex?null:h("select",{},h("option",{value:""},"Blank"),...examples.map(x=>h("option",{value:x.id},x.title)));
    const ok=await new Promise(res=>modal({title:ex?"New script from \u201c"+ex.title+"\u201d":"New script",body:h("div",{},
      h("div",{class:"frow"},h("label",{},"Name"),h("div",{class:"ctl"},h("div",{class:"rowflex",style:"gap:4px;align-items:center"},nIn,h("span",{class:"dim"},".be")),h("div",{class:"help"},"Letters, digits, _ and -; up to 36 characters."))),
      sel?h("div",{class:"frow"},h("label",{},"Start from"),h("div",{class:"ctl"},sel,h("div",{class:"help"},"An example is a complete, commented program you can run as it is and then change."))):null),
      actions:[{label:"Cancel",fn:()=>res(false)},{label:"Create",kind:"pri",fn:()=>res(true)}]}));
    if(!ok)return;
    const name=cleanName(nIn.value);if(!name){toast("Pick a name: letters, digits, _ and -","err");return;}
    const id=ex?ex.id:sel.value;let src="# "+name+".be\nlog('hello from "+name+"')\n";
    if(id){try{src=await exampleSrc(id);}catch(e){toast("Could not load the example: "+e.message,"err");return;}}
    cur=null;nameIn.value=name;ed.set(src);dirty=true;showOut("",true);paintStatus();
    if(subNav)subNav.show("editor");ed.focus();
  }
  async function save(){
    const name=cleanName(nameIn.value);if(!name){toast("Pick a name: letters, digits, _ and -","err");nameIn.focus();return false;}
    const file=name+".be",text=ed.get();
    if(!text.trim()){toast("Nothing to save","err");return false;}
    try{
      await fsPut(SCR_DIR+"/"+file,new TextEncoder().encode(text).buffer);
      if(cur&&cur!==file)await api("/api/fs/file?path="+encP(SCR_DIR+"/"+cur),{method:"DELETE"}).catch(()=>{});   /* renamed */
      cur=file;dirty=false;toast("Saved "+file,"ok");await refreshList();return true;
    }catch(e){toast("Save failed: "+e.message,"err");return false;}
  }
  function showOut(txt,ok){
    outEl.replaceChildren();hintEl.textContent="";
    if(!txt){outEl.append(h("span",{class:"dim"},"Output of Run and Check shows here."));return;}
    let first=null;
    for(const ln of txt.replace(/\n$/,"").split("\n")){
      const m=ln.match(/string:(\d+):/);
      const el=h("div",{class:(!ok&&(/^ERROR:|_error:|traceback|^\t|string:\d+:/.test(ln)))?"er":""});
      if(m){const i=ln.indexOf(m[0]);el.append(ln.slice(0,i),h("a",{href:"#",title:"Go to this line",onclick:e=>{e.preventDefault();ed.goto(Number(m[1]));}},m[0]),ln.slice(i+m[0].length));if(first==null)first=Number(m[1]);}
      else el.textContent=ln;
      outEl.append(el);
    }
    if(!ok){const hit=((ref&&ref.errors)||[]).find(e=>txt.includes(e.match));if(hit)hintEl.textContent="Hint: "+hit.hint;if(first!=null)ed.goto(first);}
  }
  async function run(){
    if(!enabled||running)return;
    const text=ed.get();if(!text.trim())return;
    running=true;paintStatus();showOut("",true);outEl.replaceChildren(h("span",{class:"dim"},"Running…"));
    const t0=Date.now();
    try{
      let body={src:text};
      if(text.length>((ref&&ref.limits&&ref.limits.src_max)||8192)-64){if(!await save())throw new Error("save first: the script is longer than the inline limit");body={name:cur};}
      const r=await api("/api/scripts/run",{method:"POST",body});
      const secs=((Date.now()-t0)/1000).toFixed(1);
      showOut((r.output||"")+(r.ok?"":"")+"\n"+(r.ok?"— finished in "+secs+" s":"— failed after "+secs+" s"),r.ok!==false);
    }catch(e){showOut("ERROR: "+e.message,false);}
    running=false;paintStatus();
  }
  async function check(){
    if(!enabled||running)return;
    const text=ed.get();if(!text.trim())return;
    try{const r=await api("/api/scripts/check",{method:"POST",body:{src:text}});showOut(r.ok?"No syntax errors.":(r.error||"syntax error"),!!r.ok);}
    catch(e){showOut("ERROR: "+e.message,false);}
  }
  async function stop(){try{await api("/api/scripts/stop",{method:"POST"});toast("Stop requested","ok");}catch(e){toast(e.message,"err");}}
  function download(){if(!cur)return;h("a",{href:API+"/api/fs/download?path="+encP(SCR_DIR+"/"+cur),download:cur}).click();}
  async function del(){
    if(!cur||!await confirmModal("Delete "+cur+" from the WiCAN?","Delete script"))return;
    try{await api("/api/fs/file?path="+encP(SCR_DIR+"/"+cur),{method:"DELETE"});toast("Deleted "+cur,"ok");cur=null;nameIn.value="";ed.set("");dirty=false;showOut("",true);await refreshList();}
    catch(e){toast("Delete failed: "+e.message,"err");}
  }
  /* ---- editor pane ---- */
  const offBanner=(!st&&!ref)?banner("crit","alert","The scripting service did not answer. Reload the page in a moment."):enabled?null:banner("warn","alert","Scripting is off: scripts can be written and saved, but not run. Turn it on under Settings below, then Submit Changes (the WiCAN restarts).");
  const installBox=window.CodeMirror?null:h("div",{class:"installbox"},"The code editor (CodeMirror 5, 210 KB, MIT) adds highlighting, line numbers, bracket matching and autocomplete. It is downloaded once by this browser and stored on the WiCAN. ",
    h("button",{class:"btn sm pri",type:"button",onclick:async()=>{if(await installResource(UI_RES.cm)){installBox.remove();ed.upgrade(ref);}}},ic("download"),"Install (210 KB)"));
  const newBtn=h("button",{class:"btn sm pri",type:"button",onclick:()=>newScript(null)},ic("plus"),"New");
  paneEd.append(offBanner,installBox,
    h("div",{class:"scr-grid"},
      h("div",{},h("div",{class:"subhead",style:"margin-top:0;justify-content:space-between"},"Stored scripts",newBtn),listEl,
        h("div",{class:"help",style:"margin-top:12px;grid-column:auto"},"Files under ",h("code",{},SCR_DIR),". A rule runs one with the action ",h("code",{},"script.run"),"; see the ",h("a",{href:"#/scripts/reference"},"Reference"),".")),
      h("div",{},bar,edHost,outEl,hintEl)));
  try{const rows=await settingsForm("script_engine",["enabled","max_runtime_ms","allow_reflash"],{advanced:["allow_reflash"]});paneEd.append(section("Settings","Scripting is off until you turn it on and submit (restart).",...rows));}
  catch(e){paneEd.append(banner("warn","alert","The scripting settings are unavailable: "+e.message));}
  ensureResource(UI_RES.cm).then(ok=>{if(ok){if(installBox)installBox.remove();ed.upgrade(ref);}});   /* not awaited: the textarea works meanwhile */
  await refreshList();
  if(st&&st.busy){running=true;paintStatus();outEl.replaceChildren(h("span",{class:"dim"},"A script is running (started elsewhere). Stop ends it."));}
  /* ---- examples pane ---- */
  paneEx.append(h("p",{class:"desc",style:"margin:0 0 12px"},"Complete, commented programs. Open one, run it as it is, then change it. They live in the firmware, so your copies are yours to edit."),
    examples.length?h("div",{class:"xcards"},...examples.map(x=>h("div",{class:"xcard"},h("h4",{},x.title),h("p",{},x.desc),
      h("div",{class:"rowflex"},chip(SCR_LEVEL[x.level]||"Example",""),x.needs?chip(SCR_NEEDS[x.needs]||x.needs,"warn"):chip("Runs anywhere","ok"),h("span",{class:"dim",style:"font-size:11px;margin-left:auto"},fmtK(x.size))),
      h("div",{class:"rowflex"},h("button",{class:"btn sm pri",type:"button",onclick:()=>newScript(x)},ic("code"),"Open in the editor"))))):h("div",{class:"empty"},"The firmware did not report any examples."));
  /* ---- reference pane ---- */
  const lim=(ref&&ref.limits)||{};
  const search=h("input",{placeholder:"filter by name or description…",style:"max-width:340px",oninput:()=>paintRef(search.value.trim().toLowerCase())});
  const refList=h("div",{});
  const insertInto=b=>{ed.insert(b.ex+"\n");if(subNav)subNav.show("editor");ed.focus();};
  function paintRef(q){
    refList.replaceChildren(...groups.map(g=>{const items=binds.filter(b=>b.group===g.id&&(!q||(b.name+" "+b.doc+" "+b.sig).toLowerCase().includes(q)));
      return items.length?h("div",{},h("div",{class:"subhead"},g.title),...items.map(b=>h("div",{class:"ref-item"},
        h("button",{class:"btn sm gh",type:"button",title:"Insert this example line into the editor",onclick:()=>insertInto(b)},ic("code"),"Insert"),
        h("div",{class:"ref-sig"},b.sig),h("div",{class:"ref-doc"},b.doc," ",h("span",{class:"dim"},"Returns "+b.ret+".")),h("div",{class:"ref-ex"},b.ex)))):null;}));
    if(!refList.childElementCount)refList.append(h("div",{class:"empty"},"Nothing matches."));
  }
  paintRef("");
  paneRef.append(
    ref?h("div",{},
      h("p",{class:"desc",style:"margin:0 0 10px"},(ref.language||"Berry")+". Inline runs up to "+fmtK(lim.src_max||8192)+", stored scripts up to "+fmtK(lim.file_max||65536)+", output up to "+fmtK(lim.out_max||4096)+" per run, sleep_ms up to "+((lim.sleep_max_ms||60000)/1000)+" s per call, runtime budget "+(lim.max_runtime_ms||0)+" ms (Settings). Responses longer than "+(lim.resp_max_bytes||128)+" bytes are cut in the returned hex."),
      section("Functions",null,search,refList),
      section("Globals a script can read",null,kv((ref.globals||[]).map(g=>[h("code",{},g.name),g.doc]))),
      section("Berry in a minute",null,h("div",{class:"prim"},...(ref.primer||[]).map(x=>h("div",{},h("b",{},x.title),h("pre",{},x.code),x.note?h("div",{class:"note"},x.note):null)))),
      section("Run a script from a rule",null,h("div",{class:"help",style:"grid-column:auto;font-size:13px;line-height:1.55"},
        h("a",{href:"#/events/rules"},"Rules & Events → Add Rule"),": pick the trigger under When, set Do to ",h("code",{},(ref.rules&&ref.rules.action)||"script.run")," and With to ",h("code",{},(ref.rules&&ref.rules.with)||'{"name": "<script>"}'),". The trigger reaches the script as the evt_* globals above; whatever the script emits with emit('script', 'done', 'value', …) is the ",h("code",{},(ref.rules&&ref.rules.event)||"script.done")," event another rule can act on (${value}). An event-triggered run blocks the rules engine while it runs, so keep those scripts short.")),
      section("When something fails",null,kv((ref.errors||[]).map(e=>[h("code",{},e.match),e.hint]))))
    :banner("warn","alert","The scripting reference is unavailable (older firmware?)."));
  return null;
};
})();
