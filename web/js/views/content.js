import { typeLabel, esc, renderIcon, prettyValue } from '../utils/formatting.js';
import { request } from '../api.js';
import { moveDashboardWidget } from '../utils/dashboard-layout.js';

export function createContentViews({ renderObjectControls, renderObjectList, renderObjectTable, renderChecklistView, renderProgressView, renderObjectCard, renderGoals, renderGoal, state, root, render }) {
  function renderPanel() {
    if (state.panel === 'templates') return renderTemplates();
    if (state.panel === 'library') return renderLibrary();
    if (state.panel === 'goals') return renderGoals();
    if (state.panel === 'schema') return renderSchema();
    if (state.panel === 'section') return renderSection();
    if (state.panel === 'history') return renderHistory();
    return renderOverview();
  }

  function renderTemplates() {
    const items=state.templates||[];
    const cards=items.length?`<div class="template-grid">${items.map(item=>`<article class="light-panel template-card"><div class="template-icon">${esc(item.icon||'📘')}</div><h3>${esc(item.name)}</h3><p>${esc(item.description||'Reusable game structure')}</p><div class="template-counts"><span>${Number(item.type_count)||0} object types</span><span>${Number(item.section_count)||0} sections</span></div><p class="dialog-help">Structure only. Objects, goals, and private game notes are not included.</p><div class="toolbar"><button class="pixel-button gold small" type="button" data-action="use-template" data-id="${esc(item.id)}">Create Game</button><button class="pixel-button small" type="button" data-action="edit-template" data-id="${esc(item.id)}">Edit</button><button class="pixel-button small" type="button" data-action="duplicate-template" data-id="${esc(item.id)}">Duplicate</button><a class="pixel-button small" href="/api/templates/${encodeURIComponent(item.id)}/export" download>Export JSON</a><button class="pixel-button small danger" type="button" data-action="delete-template" data-id="${esc(item.id)}">Delete</button></div></article>`).join('')}</div>`:'<div class="empty-state"><div class="empty-icon">📘</div><h3>NO GAME TEMPLATES YET</h3><p>Save this game’s structure to reuse its object types, formulas, sections, and selected presentation settings.</p></div>';
    return `<div class="content-heading"><div><h2>📘 GAME TEMPLATES</h2><p>Reusable structure with fresh IDs. Personal objects, goals, and notes stay with their original game.</p></div><div class="toolbar">${state.game?'<button class="pixel-button gold" type="button" data-action="save-game-template">Save Current Game</button>':''}<button class="pixel-button" type="button" data-action="import-template">Import JSON</button></div></div>${cards}`;
  }

  function renderHistory() {
    const object=state.historyObject,filters=state.historyFilters;
    if(!object)return '<div class="empty-state"><h3>OBJECT HISTORY UNAVAILABLE</h3><button class="pixel-button" data-action="history-back">Back</button></div>';
    const pageSize=20,pages=Math.max(1,Math.ceil((Number(state.historyTotal)||0)/pageSize));
    const historyFields=[...new Map((state.historyFields||[]).map(field=>[field.field_id,field])).values()];
    const numericFields=(object.fields||[]).filter(field=>['integer','decimal','percentage'].includes(field.field_type));
    const eventOptions=[['','All changes'],['created','Object created'],['updated','Object updated'],['field_type_migrated','Field type migrated'],['archived','Archived'],['restored','Restored']];
    const filtersMarkup=`<section class="object-browser light-panel history-filters" aria-label="Filter object history"><label>Change type<select data-history-filter="event_type">${eventOptions.map(([value,label])=>`<option value="${value}" ${filters.event_type===value?'selected':''}>${label}</option>`).join('')}</select></label><label>Field<select data-history-filter="field_id"><option value="">All fields</option>${historyFields.map(field=>{const current=object.fields?.find(item=>item.id===field.field_id);return `<option value="${esc(field.field_id)}" ${filters.field_id===field.field_id?'selected':''}>${esc(current?.name||field.field_name)}</option>`;}).join('')}</select></label><label>From<input type="date" data-history-filter="since" value="${esc(filters.since)}"></label><label>Through<input type="date" data-history-filter="until" value="${esc(filters.until)}"></label><label>Numeric chart<select data-history-filter="chart_field_id"><option value="">Off</option>${numericFields.map(field=>`<option value="${esc(field.id)}" ${filters.chart_field_id===field.id?'selected':''}>${esc(field.name)}</option>`).join('')}</select></label></section>`;
    const chart=filters.chart_field_id?renderHistoryChart(state.historyChart,filters.chart_field_id):'';
    const timeline=state.historyLoading
      ? '<p class="query-count" role="status">Loading object history…</p>'
      : state.historyError
        ? `<div class="empty-state" role="alert"><h3>HISTORY COULD NOT BE LOADED</h3><p>${esc(state.historyError)}</p><button class="pixel-button small" type="button" data-action="refresh-history">Try again</button></div>`
        : state.historyItems.length
          ? `<ol class="history-timeline">${state.historyItems.map(renderHistoryEvent).join('')}</ol>`
          : '<div class="empty-state"><div class="empty-icon">◷</div><h3>NO MATCHING HISTORY</h3><p>Changes to this object will appear here.</p></div>';
    return `<div class="content-heading history-heading"><div><h2>◷ OBJECT HISTORY</h2><p>${esc(object.name)} · ${esc(object.type_name||'Object')}</p></div><button class="pixel-button" type="button" data-action="history-back">← ${state.historyBackPanel==='section'?'Back to section':'Back to objects'}</button></div>${filtersMarkup}${chart}<section class="light-panel history-panel"><div class="panel-heading"><strong>${Number(state.historyTotal)||0} change${Number(state.historyTotal)===1?'':'s'}</strong><div class="toolbar"><button class="pixel-button small" type="button" data-action="history-page" data-page="${state.historyPage-1}" ${state.historyPage===0?'disabled':''}>Previous</button><span>Page ${state.historyPage+1} / ${pages}</span><button class="pixel-button small" type="button" data-action="history-page" data-page="${state.historyPage+1}" ${state.historyPage>=pages-1?'disabled':''}>Next</button></div></div>${timeline}</section>`;
  }

  function renderHistoryEvent(item) {
    const labels={created:'Object created',updated:'Object updated',field_type_migrated:'Field type migrated',archived:'Object archived',restored:'Object restored'};
    const presetChange=item.changes?.find(change=>change.field_type==='preset');
    let label=labels[item.event_type]||'Object changed';
    if(presetChange){label=presetChange.field_name.startsWith('Preset created:')?'Preset created':presetChange.field_name.startsWith('Preset deleted:')?'Preset deleted':'Preset changed';}
    const date=Number.isNaN(Date.parse(item.created_at))?item.created_at:new Date(item.created_at).toLocaleString();
    const details=item.changes?.length?`<ul>${item.changes.map(change=>`<li><strong>${esc(change.field_name)}</strong>${change.before_field_type?` <small>(${esc(change.before_field_type)} → ${esc(change.field_type)})</small>`:''}: <span>${esc(historyValue(change.before))}</span><span class="history-arrow" aria-hidden="true"> → </span><span>${esc(historyValue(change.after))}</span></li>`).join('')}</ul>`:'';
    return `<li class="history-event"><span class="history-marker" aria-hidden="true"></span><div class="history-event-body"><div class="history-event-heading"><strong>${label}</strong><time datetime="${esc(item.created_at)}">${esc(date)}</time></div>${details}</div></li>`;
  }

  function historyValue(raw) {
    if(raw==null||raw==='')return '—';
    if(typeof raw==='string')return raw;
    if(Array.isArray(raw))return raw.map(item=>String(item)).join(', ');
    if(typeof raw==='object')return JSON.stringify(raw);
    return String(raw);
  }

  function renderHistoryChart(items,fieldID) {
    const changes=[];
    for(const event of [...(items||[])].reverse())for(const change of event.changes||[]) {
      if(change.field_id!==fieldID||!['integer','decimal','percentage'].includes(change.field_type)||change.after==null)continue;
      try {const value=JSON.parse(change.after);if(typeof value==='number'&&Number.isFinite(value))changes.push({value,time:event.created_at});}catch{}
    }
    if(!changes.length)return '<section class="light-panel history-chart"><h3>FIELD PROGRESSION</h3><p>No compatible numeric values are available in the latest 100 changes.</p></section>';
    const width=620,height=190,left=42,right=16,top=18,bottom=32,chartWidth=width-left-right,chartHeight=height-top-bottom;
    const values=changes.map(item=>item.value),min=Math.min(...values),max=Math.max(...values),span=max-min||1;
    const points=changes.map((item,index)=>({x:left+(changes.length===1?chartWidth/2:index*chartWidth/(changes.length-1)),y:top+chartHeight-(item.value-min)/span*chartHeight,...item}));
    const polyline=points.map(point=>`${point.x},${point.y}`).join(' ');
    return `<section class="light-panel history-chart"><div class="panel-heading"><h3>FIELD PROGRESSION</h3><span>${esc(String(min))} – ${esc(String(max))}</span></div><svg viewBox="0 0 ${width} ${height}" role="img" aria-label="Numeric field history chart"><line x1="${left}" y1="${top+chartHeight}" x2="${width-right}" y2="${top+chartHeight}"/><line x1="${left}" y1="${top}" x2="${left}" y2="${top+chartHeight}"/><polyline points="${polyline}"/>${points.map(point=>`<circle cx="${point.x}" cy="${point.y}" r="4"><title>${esc(String(point.value))} · ${esc(new Date(point.time).toLocaleString())}</title></circle>`).join('')}<text x="${left}" y="${height-8}">${esc(new Date(points[0].time).toLocaleDateString())}</text><text x="${width-right}" y="${height-8}" text-anchor="end">${esc(new Date(points.at(-1).time).toLocaleDateString())}</text></svg></section>`;
  }

  const dashboardRequests=new Map();
  function renderOverview() {
    if(!state.dashboardLayout)return `${state.dashboardError?`<div class="dashboard-warning" role="alert">The custom dashboard could not be loaded. The regular Overview is shown below. <button class="pixel-button small" type="button" data-action="retry-dashboard">Retry</button></div>`:''}${renderLegacyOverview()}`;
    const widgets=state.dashboardLayout.widgets||[];
    if(state.dashboardEdit)return renderDashboardEditor(widgets);
    return `<div class="content-heading"><div><h2>✦ YOUR GAME SPACE</h2><p>A dashboard built from your sections, objects, goals, and uploaded images.</p></div><div class="toolbar"><button class="pixel-button gold" type="button" data-action="customize-dashboard">Customize Dashboard</button><button class="pixel-button" type="button" data-action="new-object">＋ New Object</button></div></div>${widgets.length?`<div class="dashboard-grid">${widgets.map(renderDashboardWidget).join('')}</div>`:`<div class="empty-state"><div class="empty-icon">✦</div><h3>YOUR DASHBOARD IS EMPTY</h3><p>Add widgets from your game’s own objects, fields, and goals.</p><button class="pixel-button gold" type="button" data-action="customize-dashboard">Customize Dashboard</button></div>`}`;
  }

  function renderLegacyOverview() {
    const recent = state.overview?.recent_objects || [];
    const objectCount=Number(state.overview?.object_count)||0;
    const objectContent=state.overviewLoading
      ? '<p class="query-count" role="status">Loading recent objects…</p>'
      : state.overviewError
        ? `<div class="empty-state" role="alert"><h3>RECENT OBJECTS COULD NOT BE LOADED</h3><p>${esc(state.overviewError)}</p><button class="pixel-button small" type="button" data-action="refresh-overview">Try again</button></div>`
        : objectCount===0
          ? `<div class="empty-state" style="margin:10px"><div class="empty-icon">🧰</div><h3>YOUR LIBRARY IS EMPTY</h3><p>Create an object type with the fields you care about, then add your first object.</p><button class="pixel-button gold" type="button" data-action="new-type">＋ Create Object Type</button></div>`
          : recent.length
            ? `<div class="object-grid" style="padding:9px">${recent.map(renderObjectCard).join('')}</div>`
            : '<div class="empty-state" style="margin:10px"><h3>RECENT OBJECTS ARE UNAVAILABLE</h3><p>This game has objects, but none were returned in the recent list.</p><button class="pixel-button small" type="button" data-action="refresh-overview">Try again</button></div>';
    return `<div class="content-heading"><div><h2>✦ YOUR GAME SPACE</h2><p>Your data model is yours to shape. Start with sections, types, or objects.</p></div><div class="toolbar"><button class="pixel-button gold" type="button" data-action="new-object">＋ New Object</button><button class="pixel-button" type="button" data-action="new-goal">◎ New Goal</button></div></div>
      <div class="overview-grid">
        <section class="light-panel"><div class="panel-heading"><strong>◈ OBJECT LIBRARY · ${objectCount}</strong><button class="pixel-button small" type="button" data-action="select-panel" data-panel="library">View all ${objectCount}</button></div>${objectContent}</section>
        <div style="display:grid;align-content:start;gap:10px">
          <section class="light-panel"><div class="panel-heading"><strong>▣ YOUR SECTIONS</strong><button class="pixel-button small" type="button" data-action="new-section">＋ Add</button></div>${state.sections.filter((section) => !section.archived).length ? `<div class="section-list">${state.sections.filter((section) => !section.archived).map((section) => `<button class="section-row" type="button" data-action="select-section" data-id="${esc(section.id)}"><span class="section-ic">${renderIcon(section.icon)}</span><span>${esc(section.name)}</span><span class="section-count">${Number(section.object_count) || 0}</span></button>`).join('')}</div>` : `<div class="empty-state" style="margin:9px"><h3>MAKE YOUR OWN NAVIGATION</h3><p>Sections are fully customizable and can be nested.</p></div>`}</section>
          <section class="light-panel"><div class="panel-heading"><strong>◎ GOALS & PLANS</strong><button class="pixel-button small" type="button" data-action="open-goals">View all</button></div>${state.goals.length ? `<div class="goal-list">${state.goals.slice(0,3).map(renderGoal).join('')}</div>` : `<div class="empty-state" style="margin:9px"><h3>NO GOALS YET</h3><p>Track a manual milestone, checklist, or field target.</p><button class="pixel-button small" type="button" data-action="new-goal">＋ Create Goal</button></div>`}</section>
        </div>
      </div>`;
  }

  function renderDashboardEditor(widgets) {
    const cards=widgets.map((widget,index)=>`<article class="dashboard-widget dashboard-widget-editor size-${esc(widget.size)}" draggable="true" data-dashboard-index="${index}"><div class="dashboard-widget-heading"><strong>${esc(widget.title)}</strong><span>${esc(widget.type.replaceAll('_',' '))} · ${esc(widget.size)}</span></div><p>Drag to reorder or use the accessible move buttons.</p><div class="toolbar"><button class="pixel-button small" type="button" data-action="dashboard-move" data-id="${esc(widget.id)}" data-direction="up" ${index===0?'disabled':''} aria-label="Move ${esc(widget.title)} up">↑</button><button class="pixel-button small" type="button" data-action="dashboard-move" data-id="${esc(widget.id)}" data-direction="down" ${index===widgets.length-1?'disabled':''} aria-label="Move ${esc(widget.title)} down">↓</button><label class="dashboard-size-label">Size<select data-dashboard-resize data-id="${esc(widget.id)}">${[['small','Small'],['medium','Medium'],['large','Large']].map(([size,label])=>`<option value="${size}" ${widget.size===size?'selected':''}>${label}</option>`).join('')}</select></label><button class="pixel-button small" type="button" data-action="edit-dashboard-widget" data-id="${esc(widget.id)}">Configure</button><button class="pixel-button small danger" type="button" data-action="remove-dashboard-widget" data-id="${esc(widget.id)}">Remove</button></div></article>`).join('');
    return `<div class="content-heading"><div><h2>✦ CUSTOMIZE DASHBOARD</h2><p>Layout changes stay in this draft until saved. Each widget reads current game data.</p></div><div class="toolbar"><button class="pixel-button" type="button" data-action="reset-dashboard">Reset layout</button><button class="pixel-button soft" type="button" data-action="cancel-dashboard">Cancel</button><button class="pixel-button gold" type="button" data-action="save-dashboard">Save dashboard</button></div></div><div class="toolbar dashboard-add-actions"><button class="pixel-button gold small" type="button" data-action="add-dashboard-widget">＋ Add Widget</button><span>${widgets.length} / 20 widgets</span></div>${widgets.length?`<div class="dashboard-grid dashboard-edit-grid">${cards}</div>`:'<div class="empty-state">No widgets yet. Add a widget to build this game’s Overview.</div>'}`;
  }

  function renderDashboardWidget(widget) {
    const title=esc(widget.title),config=widget.config||{};
    const header=`<div class="dashboard-widget-heading"><h3>${title}</h3>${state.dashboardEdit?`<button class="pixel-button small" type="button" data-action="edit-dashboard-widget" data-id="${esc(widget.id)}">Configure</button>`:''}</div>`;
    if(widget.type==='notes')return `<article class="dashboard-widget size-${esc(widget.size)}">${header}<div class="dashboard-note">${esc(config.text||'')}</div></article>`;
    if(widget.type==='image')return `<article class="dashboard-widget size-${esc(widget.size)}">${header}${config.asset_id?`<img class="dashboard-image" src="/api/assets/${encodeURIComponent(config.asset_id)}" loading="lazy" alt="${title}">`:'<p class="empty-state">Choose an uploaded image in widget settings.</p>'}</article>`;
    if(widget.type==='statistics')return `<article class="dashboard-widget size-${esc(widget.size)}">${header}${renderDashboardStats()}</article>`;
    if(widget.type==='recent_objects'){
      const recentData=loadDashboardWidget(widget);
      if(recentData.loading)return dashboardLoading(header,widget.size,'Loading recent objects…');
      if(recentData.error)return dashboardError(header,widget,recentData.error);
      const recent=recentData.result?.items||[];
      return `<article class="dashboard-widget size-${esc(widget.size)}">${header}${recent.length?`<div class="object-grid dashboard-object-grid">${recent.map(renderObjectCard).join('')}</div>`:`<div class="empty-state">No recent objects yet.</div>`}</article>`;
    }
    const data=loadDashboardWidget(widget);
    if(data.loading)return dashboardLoading(header,widget.size,'Loading this widget…');
    if(data.error)return dashboardError(header,widget,data.error);
    if(widget.type==='goals'){
      let goals=data.result?.items||[];if(config.goal_ids?.length){const selected=new Set(config.goal_ids);goals=goals.filter(goal=>selected.has(goal.id));}return `<article class="dashboard-widget size-${esc(widget.size)}">${header}${goals.length?`<div class="goal-list">${goals.slice(0,widgetLimit(widget,5)).map(goal=>renderGoal(goal)).join('')}</div>`:'<div class="empty-state">No goals match this widget.</div>'}</article>`;
    }
    const objects=data.result?.items||[];
    if(widget.type==='object_collection')return `<article class="dashboard-widget size-${esc(widget.size)}">${header}<p class="query-count">${Number(data.result?.total)||0} matching objects</p>${objects.length?`<div class="object-grid dashboard-object-grid">${objects.map(renderObjectCard).join('')}</div>`:'<div class="empty-state">No objects match this collection.</div>'}</article>`;
    if(widget.type==='progress')return `<article class="dashboard-widget size-${esc(widget.size)}">${header}${renderDashboardProgress(objects,config)}</article>`;
    if(widget.type==='custom_table')return `<article class="dashboard-widget size-${esc(widget.size)}">${header}${renderDashboardTable(objects,config)}</article>`;
    return `<article class="dashboard-widget size-${esc(widget.size)}">${header}<div class="empty-state">This widget type is not available.</div></article>`;
  }

  function renderDashboardStats(){
    const overview=state.overview;const metrics=[['Objects',overview?.object_count??0],['Sections',overview?.section_count??0],['Goals',state.goalStats.total||0],['Goals complete',`${state.goalStats.completed||0} / ${state.goalStats.total||0}`]];
    if(state.overviewLoading)return '<p class="query-count" role="status">Loading game statistics…</p>';
    if(state.overviewError)return `<p class="form-error" role="alert">${esc(state.overviewError)}</p>`;
    return `<div class="dashboard-stats">${metrics.map(([label,value])=>`<div class="dashboard-stat"><small>${label}</small><strong>${esc(String(value))}</strong></div>`).join('')}</div>`;
  }

  function renderDashboardTable(objects,config){
    if(!objects.length)return '<div class="empty-state">No objects are available for this table.</div>';
    const available=(objects[0].fields||[]).filter(field=>!field.archived);
    const chosen=(config.field_ids||[]).map(id=>available.find(field=>field.id===id)).filter(Boolean).slice(0,10);
    const columns=chosen.length?chosen:available.slice(0,5);
    return `<div class="table-scroll"><table class="object-table"><thead><tr><th>Object</th>${columns.map(field=>`<th>${esc(field.name)}</th>`).join('')}</tr></thead><tbody>${objects.map(object=>`<tr><td>${esc(object.name)}</td>${columns.map(field=>`<td>${esc(prettyValue(object.values?.[field.id],field,state.objects))}</td>`).join('')}</tr>`).join('')}</tbody></table></div>`;
  }

  function renderDashboardProgress(objects,config){
    const selected=new Set(config.field_ids||[]);const rows=[];
    for(const object of objects){for(const field of (object.fields||[]).filter(field=>!field.archived&&field.field_type==='progress'&&(!selected.size||selected.has(field.id)))){const value=object.values?.[field.id];if(!value||!Number.isFinite(value.current)||!Number.isFinite(value.max)||value.max<=0)continue;const percent=Math.max(0,Math.min(100,value.current/value.max*100));rows.push(`<div class="dashboard-progress-row"><span><strong>${esc(object.name)}</strong><small>${esc(field.name)} · ${esc(String(value.current))} / ${esc(String(value.max))}</small></span><div class="progress-track" role="progressbar" aria-valuenow="${Math.round(percent)}" aria-valuemin="0" aria-valuemax="100" aria-label="${esc(object.name)} ${esc(field.name)}"><div class="progress-fill" style="width:${percent}%"></div></div><b>${Math.round(percent)}%</b></div>`);}}
    return rows.length?`<div class="dashboard-progress-list">${rows.join('')}</div>`:'<div class="empty-state">Select a type or section with progress fields to show progress.</div>';
  }

  function loadDashboardWidget(widget){
    const key=`${state.game?.id}:${widget.id}:${JSON.stringify(widget.config||{})}`;
    if(state.dashboardData.has(key))return state.dashboardData.get(key);
    const entry={loading:true};state.dashboardData.set(key,entry);
    const config=widget.config||{};const limit=widgetLimit(widget,widget.type==='goals'?5:6);let url='';
    if(widget.type==='goals'){
      const params=new URLSearchParams({limit:String(Math.min(limit,100)),offset:'0',status:config.status||'active',sort:config.sort||'updated_at',direction:config.direction||'desc'});url=`/api/games/${encodeURIComponent(state.game.id)}/goals?${params}`;
    }else{
      const params=new URLSearchParams({limit:String(limit),offset:'0',sort:widget.type==='recent_objects'?'updated_at':config.sort||'updated_at',direction:widget.type==='recent_objects'?'desc':config.direction||'desc'});if(config.type_id)params.set('type_id',config.type_id);if(config.section_id)params.set('section_id',config.section_id);url=`/api/games/${encodeURIComponent(state.game.id)}/objects?${params}`;
    }
    const gameID=state.game?.id;
    const controller=new AbortController();
    const timeout=setTimeout(()=>controller.abort(),20000);
    request(url,{signal:controller.signal}).then(result=>{entry.loading=false;entry.result=result;}).catch(error=>{entry.loading=false;entry.error=error?.name==='AbortError'?'This widget took too long to load. Check your connection and try again.':error.message;}).finally(()=>{clearTimeout(timeout);if(state.game?.id===gameID&&state.panel==='overview'&&!state.dashboardEdit)render();});
    return entry;
  }

  function widgetLimit(widget,fallback){const value=Number(widget.config?.limit);return Number.isInteger(value)&&value>0?Math.min(value,30):fallback;}
  function dashboardLoading(header,size,message){return `<article class="dashboard-widget size-${esc(size)}">${header}<p class="query-count" role="status">${message}</p></article>`;}
  function dashboardError(header,widget,message){return `<article class="dashboard-widget size-${esc(widget.size)}">${header}<div class="empty-state" role="alert">${esc(message)}<button class="pixel-button small" type="button" data-action="retry-dashboard-widget" data-id="${esc(widget.id)}">Retry</button></div></article>`;}
  function dashboardKey(widget){return `${state.game?.id}:${widget.id}:${JSON.stringify(widget.config||{})}`;}
  function retryDashboardWidget(widgetID){const widget=state.dashboardLayout?.widgets?.find(item=>item.id===widgetID);if(widget)state.dashboardData.delete(dashboardKey(widget));render();}

  root.addEventListener('dragstart',event=>{const card=event.target.closest('[data-dashboard-index]');if(!card||!state.dashboardEdit)return;event.dataTransfer?.setData('text/plain',card.dataset.dashboardIndex);if(event.dataTransfer)event.dataTransfer.effectAllowed='move';});
  root.addEventListener('dragover',event=>{if(event.target.closest('[data-dashboard-index]')&&state.dashboardEdit)event.preventDefault();});
  root.addEventListener('drop',event=>{const card=event.target.closest('[data-dashboard-index]');if(!card||!state.dashboardEdit)return;event.preventDefault();const from=Number(event.dataTransfer?.getData('text/plain')),to=Number(card.dataset.dashboardIndex);if(Number.isInteger(from)&&Number.isInteger(to)&&from!==to){const widget=state.dashboardLayout.widgets[from];if(widget){moveDashboardWidget(state.dashboardLayout,widget.id,to-from);render();}}});
  function renderLibrary() {
    return `<div class="content-heading"><div><h2>◈ OBJECT LIBRARY</h2><p>Browse every active object, including objects without a section.</p></div><div class="toolbar"><button class="pixel-button" type="button" data-action="compare-selected" ${state.compareSelection.length<2?'disabled':''}>Compare selected (${state.compareSelection.length}/4)</button><button class="pixel-button gold" type="button" data-action="new-object">＋ New Object</button></div></div>${renderObjectControls()}<div id="library-results">${renderLibraryResults()}</div>`;
  }

  function renderLibraryResults() {
    const pages=Math.max(1,Math.ceil((Number(state.objectTotal)||0)/50));state.objectPage=Math.min(state.objectPage,pages-1);
    return `${state.objectQueryError?`<p class="form-error" role="alert">${esc(state.objectQueryError)}</p>`:''}<div class="library-pagination"><span>${Number(state.objectTotal)||0} objects · page ${state.objectPage+1} / ${pages}</span><button class="pixel-button small" type="button" data-action="library-page" data-page="${state.objectPage-1}" ${state.objectPage===0?'disabled':''}>Previous</button><button class="pixel-button small" type="button" data-action="library-page" data-page="${state.objectPage+1}" ${state.objectPage===pages-1?'disabled':''}>Next</button></div>${state.objects.length?`<div class="object-grid">${state.objects.map((object)=>renderObjectCard(object)).join('')}</div>`:'<div class="empty-state">No objects match these filters.</div>'}`;
  }

  function refreshLibraryResults(){const results=root.querySelector('#library-results');if(results)results.innerHTML=renderLibraryResults();}

  function renderSection() {
    const section = state.sections.find((item) => item.id === state.sectionId);
    if (!section) return renderOverview();
    const objects = state.objects;
    const linkedCount=Number(section.object_count)||0;
    const sectionGoals=state.sectionGoals||[];
    const archived = state.sections.filter((item) => item.parent_id === section.id && !item.archived);
    return `<div class="content-heading"><div><h2>${renderIcon(section.icon)} ${esc(section.name)}</h2><p>${linkedCount} linked object${linkedCount === 1 ? '' : 's'} · ${esc(section.display_style)} view</p></div><div class="toolbar"><button class="pixel-button" type="button" data-action="compare-selected" ${state.compareSelection.length<2?'disabled':''}>Compare (${state.compareSelection.length}/4)</button><button class="pixel-button" type="button" data-action="add-existing-goal">＋ Goal</button><button class="pixel-button gold" type="button" data-action="add-existing">＋ Add Existing</button><button class="pixel-button" type="button" data-action="new-object">＋ Create Object</button><button class="pixel-button soft" type="button" data-action="edit-section" data-id="${esc(section.id)}">✎</button><button class="pixel-button danger small" type="button" data-action="archive-section" data-id="${esc(section.id)}" title="Archive section">Archive</button></div></div>
      ${sectionGoals.length?`<section class="light-panel goal-list section-goals">${sectionGoals.map((goal)=>renderGoal(goal,true)).join('')}</section>`:''}
      ${archived.length ? `<div class="section-list light-panel" style="margin-bottom:10px">${archived.map((child) => `<button class="section-row" type="button" data-action="select-section" data-id="${esc(child.id)}">${renderIcon(child.icon)} ${esc(child.name)}<span class="section-count">Nested section</span></button>`).join('')}</div>` : ''}
      ${renderObjectControls(section)}<div id="section-object-results">${renderSectionObjects(objects,section)}</div>`;
  }

  function renderSectionObjects(objects,section) {
    const total=Number(state.objectTotal)||0,pages=Math.max(1,Math.ceil(total/50));
    const pagination=total?`<div class="library-pagination"><span>${total} matching objects · page ${state.objectPage+1} / ${pages}</span><button class="pixel-button small" type="button" data-action="section-page" data-page="${state.objectPage-1}" ${state.objectPage===0?'disabled':''}>Previous</button><button class="pixel-button small" type="button" data-action="section-page" data-page="${state.objectPage+1}" ${state.objectPage===pages-1?'disabled':''}>Next</button></div>`:'';
    if(state.objectQueryError)return `${pagination}<p class="form-error" role="alert">${esc(state.objectQueryError)}</p>`;
    if(!objects.length&&total===0&&Number(section.object_count)>0)return `${pagination}<p class="query-count" role="status">0 matching objects</p><div class="empty-state">No objects match these filters. Change the conditions or clear the view.</div>`;
    if(objects.length)return `${pagination}<p class="query-count" role="status">${total} matching objects</p>${section.display_style==='list'?renderObjectList(objects,section):section.display_style==='table'?renderObjectTable(objects,section):section.display_style==='checklist'?renderChecklistView(objects,section):section.display_style==='progress'?renderProgressView(objects,section):`<div class="object-grid">${objects.map((object)=>renderObjectCard(object,section)).join('')}</div>`}`;
    return `<div class="empty-state"><div class="empty-icon">🗂️</div><h3>THIS SECTION IS READY</h3><p>Add an existing object here or create a new one. The same object can live in several sections.</p><button class="pixel-button gold" type="button" data-action="add-existing">＋ Add Existing Object</button></div>`;
  }

  root.addEventListener('object-query-changed',()=>{
    if(state.panel==='library')refreshLibraryResults();
    if(state.panel==='section') {
      const section=state.sections.find(item=>item.id===state.sectionId);
      const target=root.querySelector('#section-object-results');
      if(section&&target)target.innerHTML=renderSectionObjects(state.objects,section);
    }
  });

  function renderSchema() {
    const types = state.types;
    return `<div class="content-heading"><div><h2>⚙ YOUR CUSTOM DATA TYPES</h2><p>Fields belong to a type and stay editable on each object. Use the migration wizard to change a field type safely.</p></div><button class="pixel-button gold" type="button" data-action="new-type">＋ New Type</button></div>
      ${types.length ? `<div class="schema-grid">${types.map((type) => `<article class="schema-card"><h3><span>${renderIcon(type.icon||'◈')}</span>${esc(type.name)} ${type.archived?'(archived)':''}</h3>${type.image?`<img class="type-image" src="${esc(type.image)}" alt="">`:''}<p>${esc(type.description || 'Custom objects in this game.')}</p><div class="field-editor-list">${(type.fields||[]).map((field) => `<div class="field-row ${field.archived?'field-archived':''}"><span class="field-icon">${renderIcon(field.icon||'✦')}</span><strong>${esc(field.name)}</strong><span class="field-kind">${esc(typeLabel(field.field_type))}</span>${field.archived?`<button class="pixel-button small" type="button" data-action="restore-field" data-id="${esc(field.id)}">Restore</button>`:`<button class="pixel-button small soft" type="button" data-action="edit-field" data-id="${esc(field.id)}">Edit</button><button class="pixel-button small" type="button" data-action="change-field-type" data-id="${esc(field.id)}">Change Type</button><button class="pixel-button small danger" type="button" data-action="archive-field" data-id="${esc(field.id)}" title="Archive field; values are preserved">Archive</button>`}</div>`).join('')}</div><div class="toolbar" style="margin-top:9px">${type.archived?`<button class="pixel-button small" type="button" data-action="restore-type" data-id="${esc(type.id)}">Restore type</button>`:`<button class="pixel-button small" type="button" data-action="new-field" data-type="${esc(type.id)}">＋ Add Field</button><button class="pixel-button small" type="button" data-action="edit-type" data-id="${esc(type.id)}">Edit type</button><button class="pixel-button small danger" type="button" data-action="archive-type" data-id="${esc(type.id)}">Archive type</button>`}</div></article>`).join('')}</div>` : `<div class="empty-state"><div class="empty-icon">🧩</div><h3>DESIGN YOUR OWN DATA</h3><p>Create a type and define characteristics that make sense for your game. No game comes with preset fields.</p><button class="pixel-button gold" type="button" data-action="new-type">＋ Create Object Type</button></div>`}`;
  }

  function renderWelcome() {
    return `<div class="game-hero"><div class="hero-symbol">✧</div><div class="hero-copy"><h1 class="hero-title">YOUR PERSONAL GAME TRACKER</h1><p class="hero-description">A cozy place for every build, quest, collection, and idea.</p></div></div><div class="content"><div class="empty-state" style="padding:48px 18px"><div class="empty-icon">🎮 ✦</div><h3>YOUR ADVENTURE BEGINS HERE</h3><p>Create your first game. Every section, object type, field, and goal after that is yours to define.</p><button class="pixel-button gold" type="button" data-action="new-game">＋ Add Your First Game</button></div></div>`;
  }

  return { renderPanel, renderTemplates, renderOverview, retryDashboardWidget, renderHistory, renderLibrary, renderLibraryResults, refreshLibraryResults, renderSection, renderSchema, renderWelcome };
}
