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
  if(remembered){state.panel=remembered.panel||'overview';state.sectionId=remembered.sectionId||null;state.expanded=new Set(remembered.expanded||[]);}
  let generation=0;
  async function load() {
    const version=++generation;
    const games=await getItems('/api/games');if(version!==generation)return;state.games=games;
    if(!state.game&&remembered?.gameID)state.game=games.find(game=>game.id===remembered.gameID)||null;
    remembered=null;
    if (state.game && !state.games.some((game) => game.id === state.game.id)) state.game = null;
    if (!state.game && state.games.length) state.game = state.games[0];
    if (!state.game) { state.sections = []; state.types = []; state.objects = []; state.goals = []; render(); return; }
    const gameID = state.game.id;
    const [freshGame, sections, types, objects, goals] = await Promise.all([
      request(`/api/games/${gameID}`),
      getItems(`/api/games/${gameID}/sections?archived=true`),
      getItems(`/api/games/${gameID}/object-types?archived=true`),
      getItems(`/api/games/${gameID}/objects`),
      getItems(`/api/games/${gameID}/goals`),
    ]);
    if(version!==generation)return;
    if (state.game?.id === gameID) {
      state.game = freshGame;
      state.sections = sections;
      state.types = types;
      state.objects = objects;
      state.goals = goals;
      if (state.sectionId && !sections.some((section) => section.id === state.sectionId && !section.archived)) state.sectionId = null;
    }
    render();
  }

  function render() {
    try {localStorage.setItem(navigationKey,JSON.stringify({gameID:state.game?.id,panel:state.panel,sectionId:state.sectionId,expanded:[...state.expanded]}));}catch{}
    const totalObjects = state.objects.length;
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
            <div class="sidebar-spacer"></div>
            <section class="quick-stats"><h3>▥ QUICK STATS</h3><div class="stat-row"><span>✦ Games</span><strong>${state.games.length}</strong></div><div class="stat-row"><span>◈ Objects</span><strong>${totalObjects}</strong></div><div class="stat-row"><span>▣ Sections</span><strong>${state.sections.filter((item) => !item.archived).length}</strong></div><div class="stat-row"><span>◎ Goals done</span><strong>${state.goals.filter((goal) => goal.completed).length} / ${state.goals.length}</strong></div></section>
          </aside>
          <main class="main">
            ${state.game ? renderGame() : renderWelcome()}
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
  const context = { root, state, load, render, openForm, fieldRow, toast, run };
  for (const name of ["renderObjectList", "renderObjectTable", "renderChecklistView", "renderProgressView", "objectProgress", "renderObjectCard", "renderGoals", "renderGoal", "renderPanel", "renderOverview", "renderLibrary", "renderLibraryResults", "refreshLibraryResults", "renderSection", "renderSchema", "renderWelcome", "renderGameList", "renderGame", "tab", "openGameForm", "openSectionForm", "openTypeForm", "openAddExisting", "openAddExistingGoal", "openFieldForm", "renderChoiceMigrations", "fieldOptionsHTML", "openObjectForm", "renderFieldInput", "readValues", "openGoalForm", "renderGoalSpecific", "addConditionRow", "updateConditionRow", "readGoalConditions", "openAddTask", "openProfile", "submitForm", "uploadImage"]) context[name] = (...args) => actions[name](...args);
  const { renderGameList, renderGame, renderWelcome, renderPanel } = context;
  for (const name of ['getObjectView','filterObjects','renderObjectControls','readGoalTasks']) context[name] = (...args) => actions[name](...args);
  Object.assign(actions, createObjectBrowser(context));
  for (const name of ['renderSectionTree','moveSection','openMoveSection','shiftSection']) context[name] = (...args) => actions[name](...args);
  Object.assign(actions, createSectionTree(context));
  Object.assign(actions, createObjectViews(context));
  Object.assign(actions, createGoalViews(context));
  Object.assign(actions, createContentViews(context));
  Object.assign(actions, createNavigation(context));
  Object.assign(actions, createGameEditors(context));
  Object.assign(actions, createFieldEditor(context));
  Object.assign(actions, createObjectEditor(context));
  Object.assign(actions, createGoalEditor(context));
  Object.assign(actions, createAccountEditor(context));
  Object.assign(actions, createActions(context));
  root.addEventListener('click',event=>{
    const button=event.target.closest('[data-icon-choice]');if(!button)return;
    const input=button.closest('.form-field').querySelector('input[name=icon]');
    input.value=button.dataset.iconChoice;input.dispatchEvent(new Event('input',{bubbles:true}));
  });
  bindWorkspaceEvents(context);
  load().catch((error)=>{root.innerHTML=`<main class="auth-screen"><section class="auth-card"><div class="auth-inner"><h1 class="auth-title">COULD NOT OPEN THE LIBRARY</h1><p class="auth-copy">${esc(error.message)}</p><button class="pixel-button gold" type="button" data-action="reload">TRY AGAIN</button></div></section></main>`;});
}
