import { createObjectBrowser } from '../components/object-browser.js';
import { createSectionTree } from '../components/section-tree.js';
import { getItems, request } from '../api.js';
import { ensureDialog, openModal } from '../modal.js';
import { esc, iconUploadInput } from '../utils/formatting.js';
import { createWorkspaceState } from './state.js';
import { bindWorkspaceEvents } from './events.js';
import { createObjectViews } from '../components/object-views.js';
import { createGoalViews } from '../views/goals.js';
import { createContentViews } from '../views/content.js';
import { createNavigation } from './navigation.js';
import { createGameEditors } from '../editors/game-editor.js';
import { createPresetEditor } from '../editors/preset-editor.js';
import { createDashboardEditor } from '../editors/dashboard-editor.js';
import { createGalleryEditor } from '../editors/gallery-editor.js';
import { createComparisonEditor } from '../editors/comparison-editor.js';
import { createRelationshipGraphEditor } from '../editors/relationship-graph-editor.js';
import { createFieldEditor } from '../editors/field-editor.js';
import { createObjectEditor } from '../editors/object-editor.js';
import { createGoalEditor } from '../editors/goal-editor.js';
import { createAccountEditor } from '../editors/account-editor.js';
import { createActions } from './actions.js';

export function initWorkspace(root, user) {
  const state = createWorkspaceState(user);
  const navigationKey=`gametracker-navigation:${user.id}`;
  let remembered=null;
  try {remembered=JSON.parse(localStorage.getItem(navigationKey));}catch{}
  if(remembered){state.panel=remembered.panel==='history'?'library':remembered.panel||'overview';state.sectionId=state.panel==='section'?remembered.sectionId||null:null;state.expanded=new Set(remembered.expanded||[]);}
  let generation=0;
  let overviewSequence=0,overviewAbort=null;
  let historySequence=0,historyAbort=null;
  async function refreshOverview(gameID=state.game?.id, renderOnFinish=true) {
    if(!gameID){overviewSequence++;overviewAbort?.abort();state.overview=null;state.overviewLoading=false;state.overviewError='';return;}
    const sequence=++overviewSequence;
    overviewAbort?.abort();
    overviewAbort=new AbortController();
    state.overview=null;state.overviewLoading=true;state.overviewError='';
    if(renderOnFinish&&state.panel==='overview')render();
    try {
      const result=await request(`/api/games/${encodeURIComponent(gameID)}/overview`,{signal:overviewAbort.signal});
      if(sequence!==overviewSequence||state.game?.id!==gameID)return;
      state.overview=result;
    } catch(error) {
      if(error?.name==='AbortError'||sequence!==overviewSequence||state.game?.id!==gameID)return;
      state.overviewError=error.message;
    } finally {
      if(sequence===overviewSequence&&state.game?.id===gameID){
        state.overviewLoading=false;
        if(renderOnFinish&&state.panel==='overview')render();
      }
    }
  }
  async function refreshHistory() {
    const object=state.historyObject;
    if(!object||state.panel!=='history')return;
    const sequence=++historySequence;
    historyAbort?.abort();historyAbort=new AbortController();
    const signal=historyAbort.signal,filters=state.historyFilters;
    const params=new URLSearchParams({limit:'20',offset:String(state.historyPage*20)});
    if(filters.event_type)params.set('event_type',filters.event_type);
    if(filters.field_id)params.set('field_id',filters.field_id);
    if(filters.since)params.set('since',`${filters.since}T00:00:00.000000000Z`);
    if(filters.until)params.set('until',`${filters.until}T23:59:59.999999999Z`);
    const chartParams=new URLSearchParams({limit:'100',offset:'0'});
    if(filters.chart_field_id)chartParams.set('field_id',filters.chart_field_id);
    if(filters.since)chartParams.set('since',`${filters.since}T00:00:00.000000000Z`);
    if(filters.until)chartParams.set('until',`${filters.until}T23:59:59.999999999Z`);
    state.historyLoading=true;state.historyError='';
    render();
    try {
      const base=`/api/objects/${encodeURIComponent(object.id)}/history`;
      const [result,chart]=await Promise.all([
        request(`${base}?${params}`,{signal}),
        filters.chart_field_id?request(`${base}?${chartParams}`,{signal}):Promise.resolve({items:[]}),
      ]);
      if(sequence!==historySequence||state.panel!=='history'||state.historyObject?.id!==object.id||state.game?.id!==object.game_id)return;
      state.historyItems=result.items||[];state.historyFields=result.fields||[];state.historyTotal=Number(result.total)||0;
      state.historyChart=chart.items||[];
    } catch(error) {
      if(error?.name==='AbortError'||sequence!==historySequence)return;
      state.historyError=error.message;
    } finally {
      if(sequence===historySequence&&state.panel==='history'&&state.historyObject?.id===object.id){
        state.historyLoading=false;render();
      }
    }
  }
  async function load() {
    const version=++generation;
    const [games,templates]=await Promise.all([getItems('/api/games'),getItems('/api/templates')]);if(version!==generation)return;state.games=games;state.templates=templates;
    if(!state.game&&remembered?.gameID)state.game=games.find(game=>game.id===remembered.gameID)||null;
    remembered=null;
    if (state.game && !state.games.some((game) => game.id === state.game.id)) state.game = null;
    if (!state.game && state.games.length) state.game = state.games[0];
    if (!state.game) { await refreshOverview(null,false);state.dashboardLayout=null;state.dashboardError='';state.dashboardData.clear();state.sections = []; state.types = []; state.objects = []; state.objectCache.clear(); state.objectTotal=0; state.gameObjectTotal=0; state.goals = []; state.sectionGoals=[];state.goalCache.clear();state.goalStats={total:0,completed:0};render(); return; }
    const gameID = state.game.id;
    const overviewRequest=refreshOverview(gameID,false);
    const [freshGame, sections, types, dashboard] = await Promise.all([
      request(`/api/games/${gameID}`),
      getItems(`/api/games/${gameID}/sections?archived=true`),
      getItems(`/api/games/${gameID}/object-types?archived=true`),
      request(`/api/games/${gameID}/dashboard`).catch(error=>({layout:null,error:error.message})),
    ]);
    if(version!==generation)return;
    if (state.sectionId && !sections.some((section) => section.id === state.sectionId && !section.archived)) state.sectionId = null;
    const goalParams=new URLSearchParams({sort:'updated_at',direction:'desc',limit:'3',offset:'0'});
    const sectionGoalsURL=state.panel==='section'&&state.sectionId?`/api/games/${gameID}/goals?section_id=${encodeURIComponent(state.sectionId)}`:'';
    const [goalOverview,sectionGoals]=await Promise.all([
      request(`/api/games/${gameID}/goals?${goalParams}`),
      sectionGoalsURL?getItems(sectionGoalsURL):Promise.resolve([]),
    ]);
    if(version!==generation)return;
    if (state.game?.id === gameID) {
      state.game = freshGame;
      state.sections = sections;
      state.types = types;
      state.dashboardLayout=dashboard.layout||null;state.dashboardError=dashboard.error||'';state.dashboardData.clear();
      state.goals = goalOverview.items||[];
      state.sectionGoals = sectionGoals;
      state.goalCache = new Map([...state.goals,...sectionGoals].map(goal=>[goal.id,goal]));
      state.goalStats = {total:Number(goalOverview.game_total)||0,completed:Number(goalOverview.completed_total)||0};
      state.objectCache.clear();
    }
    await overviewRequest;
    if(version!==generation)return;
    await actions.refreshObjects(false);
    if(version!==generation)return;
    render();
  }

  function render() {
    try {localStorage.setItem(navigationKey,JSON.stringify({gameID:state.game?.id,panel:state.panel==='history'?state.historyBackPanel:state.panel,sectionId:state.panel==='history'?state.historyBackSection:state.sectionId,expanded:[...state.expanded]}));}catch{}
    const dialog=root.querySelector('#app-modal');
    const markup = `
      <div class="app-frame">
        <header class="topbar">
          <div class="brand"><div class="brand-mark" aria-hidden="true">🎮</div><div><div class="brand-title">GAME TRACKER ✦</div><div class="brand-subtitle">✦ Track · Plan · Explore · Collect ✦</div></div></div>
          <div class="sky-strip" aria-hidden="true"></div>
          <div class="profile-box"><div class="avatar" aria-hidden="true">🧙</div><div class="profile-meta"><div class="profile-name">${esc(state.user.username)}</div><div class="profile-caption">♛ My Profile</div></div><button class="icon-button" type="button" data-action="logout" title="Sign out" aria-label="Sign out">↪</button><button class="icon-button" type="button" data-action="profile" title="Account details" aria-label="Account details">⚙</button></div>
        </header>
        <div class="layout">
          <aside class="sidebar" aria-label="Game library">
            <div class="sidebar-heading"><span>✦ MY GAMES</span><span>${state.games.length}</span></div>
            <label class="visually-hidden" for="game-search">Search games</label>
            <input id="game-search" class="game-search" type="search" placeholder="Find a game…" value="${esc(state.search)}">
            <div class="game-list">${renderGameList()}</div>
            <button class="pixel-button" type="button" data-action="new-game">＋ Add Game</button>
            <button class="pixel-button" type="button" data-action="select-panel" data-panel="templates">📘 Game Templates</button>
            <div class="sidebar-spacer"></div>
            <section class="quick-stats"><h3>▥ QUICK STATS</h3><div class="stat-row"><span>✦ Games</span><strong>${state.games.length}</strong></div><div class="stat-row"><span>◈ Objects</span><strong>${Number(state.gameObjectTotal)||0}</strong></div><div class="stat-row"><span>▣ Sections</span><strong>${state.sections.filter((item) => !item.archived).length}</strong></div><div class="stat-row"><span>◎ Goals done</span><strong>${state.goalStats.completed} / ${state.goalStats.total}</strong></div></section>
          </aside>
          <main class="main">
            ${state.game ? renderGame() : state.panel==='templates' ? `<main class="main"><section class="content">${renderPanel()}</section></main>` : renderWelcome()}
          </main>
        </div>
      </div>`;
    for(const child of [...root.children])if(child!==dialog)child.remove();
    root.insertAdjacentHTML('afterbegin',markup);
    ensureDialog(root);
  }

  function openForm(title, body, submit='SAVE') { openModal(ensureDialog(root),title,`<form id="modal-form" autocomplete="off">${body}</form>`,submit); }

  function fieldRow(label,name,value='',type='text',wide=false,extra='') { const icons=name==='icon'?` list="pixel-icon-options"`:'';const suggestions=name==='icon'?`<datalist id="pixel-icon-options"><option value="✦"><option value="❤"><option value="⚔"><option value="★"><option value="♛"><option value="◈"><option value="⌖"><option value="⚑"><option value="✿"><option value="☀"><option value="◎"><option value="🗺️"><option value="🎮"><option value="🛡️"></datalist>`:'';return `<div class="form-field ${wide?'wide':''}"><label for="f-${esc(name)}">${esc(label)}</label><input id="f-${esc(name)}" name="${esc(name)}" type="${type}" value="${esc(value??'')}"${icons} ${extra}>${suggestions}${name==='icon'?`<div class="icon-palette" role="group" aria-label="Suggested icons">${['✦','❤','⚔','★','♛','◈','⌖','⚑','✿','☀','◎','🗺️','🎮','🛡️'].map(icon=>`<button type="button" class="pixel-button small soft" data-icon-choice="${icon}" aria-label="Use ${icon} icon">${icon}</button>`).join('')}</div>${iconUploadInput()}`:''}</div>`; }

  function toast(message,isError=false) {
    const region=document.querySelector('#toast-region');if(!region)return;const item=document.createElement('div');item.className=`toast${isError?' error':''}`;item.textContent=message;region.append(item);setTimeout(()=>item.remove(),3600);
  }

  async function run(action) {
    try { await action(); } catch(error) { toast(error.message,true); }
  }

  // Late-bound callbacks keep module dependencies one-way; state stays private to this workspace.
  const actions = {};
  const context = { root, state, load, render, refreshOverview, refreshHistory, openForm, fieldRow, toast, run };
  for (const name of ["renderObjectList", "renderObjectTable", "renderChecklistView", "renderProgressView", "objectProgress", "renderObjectCard", "renderGoals", "renderGoal", "renderPanel", "renderOverview", "retryDashboardWidget", "renderHistory", "renderLibrary", "renderLibraryResults", "refreshLibraryResults", "refreshObjects", "refreshHistory", "renderSection", "renderSchema", "renderWelcome", "renderGameList", "renderGame", "tab", "openGameForm", "openTemplateForm", "openTemplateImportForm", "openDashboardWidgetForm", "readDashboardWidget", "defaultDashboard", "widgetTypes", "openSectionForm", "openTypeForm", "openAddExisting", "openAddExistingGoal", "openFieldForm", "openFieldTypeMigration", "migrationRequest", "renderMigrationPreview", "renderChoiceMigrations", "fieldOptionsHTML", "openObjectForm", "openObjectGallery", "openObjectComparison", "openObjectGraph", "openObjectPresets", "openPresetForm", "activeObject", "currentPreset", "renderFieldInput", "readValues", "openGoalForm", "openGoalDependencies", "renderGoalSpecific", "addConditionRow", "updateConditionRow", "readGoalConditions", "openAddTask", "openProfile", "submitForm", "uploadImage"]) context[name] = (...args) => actions[name](...args);
  const { renderGameList, renderGame, renderWelcome, renderPanel } = context;
  for (const name of ['getObjectView','renderObjectControls','readGoalTasks']) context[name] = (...args) => actions[name](...args);
  Object.assign(actions, createObjectBrowser(context));
  for (const name of ['renderSectionTree','moveSection','openMoveSection','shiftSection']) context[name] = (...args) => actions[name](...args);
  Object.assign(actions, createSectionTree(context));
  Object.assign(actions, createObjectViews(context));
  Object.assign(actions, createGoalViews(context));
  Object.assign(actions, createContentViews(context));
  Object.assign(actions, createNavigation(context));
  Object.assign(actions, createGameEditors(context));
  Object.assign(actions, createDashboardEditor(context));
  Object.assign(actions, createGalleryEditor(context));
  Object.assign(actions, createComparisonEditor(context));
  Object.assign(actions, createRelationshipGraphEditor(context));
  Object.assign(actions, createFieldEditor(context));
  Object.assign(actions, createObjectEditor(context));
  Object.assign(actions, createPresetEditor(context));
  Object.assign(actions, createGoalEditor(context));
  Object.assign(actions, createAccountEditor(context));
  Object.assign(actions, createActions(context));
  root.addEventListener('click',event=>{
    const button=event.target.closest('[data-icon-choice]');if(!button)return;
    const input=button.closest('.form-field').querySelector('input[name=icon]');
    input.value=button.dataset.iconChoice;input.dispatchEvent(new Event('input',{bubbles:true}));
  });
  bindWorkspaceEvents(context);
  const showLoadError=(error)=>{
    root.innerHTML=`<main class="auth-screen"><section class="auth-card"><div class="auth-inner"><h1 class="auth-title">COULD NOT OPEN THE LIBRARY</h1><p class="auth-copy" role="alert">${esc(error.message)}</p><button class="pixel-button gold" type="button" data-retry-workspace>TRY AGAIN</button></div></section></main>`;
    root.querySelector('[data-retry-workspace]').addEventListener('click',()=>load().catch(showLoadError));
  };
  load().catch(showLoadError);
}
