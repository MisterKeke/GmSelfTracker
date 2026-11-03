import { getItems, request, setCSRF } from './api.js';
import { closeModal, ensureDialog, openModal } from './modal.js';

const FIELD_TYPES = [
  ['text','Text'],['long_text','Long text'],['integer','Integer'],['decimal','Decimal number'],['percentage','Percentage'],['boolean','Checkbox'],['select','Select'],['date','Date'],['progress','Progress'],['reference','Single object reference'],['references','Multiple object references'],['formula','Calculated formula']
];

export function initWorkspace(root, user) {
  const state = { user, games: [], game: null, sections: [], types: [], objects: [], goals: [], panel: 'overview', sectionId: null, search: '' };

  async function load() {
    state.games = await getItems('/api/games');
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
    const totalObjects = state.objects.length;
    root.innerHTML = `
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
    ensureDialog(root);
  }

  function renderGameList() {
    const games = state.games.filter((game) => game.name.toLowerCase().includes(state.search.toLowerCase()));
    if (!games.length) return `<p class="search-empty">${state.search ? 'No game matches.' : 'Your library is waiting.'}</p>`;
    return games.map((game, index) => `<button type="button" class="game-link ${state.game?.id === game.id ? 'active' : ''}" data-action="select-game" data-id="${esc(game.id)}"><span class="game-symbol">${game.cover_image ? `<img src="${esc(game.cover_image)}" alt="" style="width:100%;height:100%;object-fit:cover">` : ['🎮','🗺️','⚔️','🌿'][index % 4]}</span><span class="game-link-title">${esc(game.name)}</span></button>`).join('');
  }

  function renderGame() {
    return `
      <section class="game-hero">
        <div class="hero-symbol">${state.game.cover_image ? `<img src="${esc(state.game.cover_image)}" alt="" style="width:100%;height:100%;object-fit:cover">` : '✧'}</div>
        <div class="hero-copy"><h1 class="hero-title">${esc(state.game.name)}</h1><p class="hero-description">${esc(state.game.description || 'Your personal game progress tracker')}</p></div>
        <div class="hero-actions"><button class="pixel-button small" type="button" data-action="edit-game">✎ Edit</button></div>
      </section>
      <nav class="tabbar" aria-label="Game workspace">
        ${tab('overview','✦ Overview')}
        ${renderSectionTabs()}
        <button class="tab add-tab" type="button" data-action="new-section">＋ Section</button>
        ${tab('goals','◎ Goals')}${tab('schema','⚙ Types & Fields')}
      </nav>
      <section class="content">${renderPanel()}</section>`;
  }

  function renderSectionTabs() {
    return state.sections.filter((section) => !section.archived).map((section) => {
      const depth = sectionDepth(section, state.sections);
      return `<button type="button" class="tab ${state.panel === 'section' && state.sectionId === section.id ? 'active' : ''}" style="padding-left:${12 + Math.min(depth,3)*8}px" data-action="select-section" data-id="${esc(section.id)}">${esc(section.icon)} ${esc(section.name)}</button>`;
    }).join('');
  }

  function tab(panel, label) { return `<button class="tab ${state.panel === panel ? 'active' : ''}" type="button" data-action="select-panel" data-panel="${panel}">${label}</button>`; }

  function renderPanel() {
    if (state.panel === 'goals') return renderGoals();
    if (state.panel === 'schema') return renderSchema();
    if (state.panel === 'section') return renderSection();
    return renderOverview();
  }

  function renderOverview() {
    const recent = [...state.objects].slice(0, 6);
    return `<div class="content-heading"><div><h2>✦ YOUR GAME SPACE</h2><p>Your data model is yours to shape. Start with sections, types, or objects.</p></div><div class="toolbar"><button class="pixel-button gold" type="button" data-action="new-object">＋ New Object</button><button class="pixel-button" type="button" data-action="new-goal">◎ New Goal</button></div></div>
      <div class="overview-grid">
        <section class="light-panel"><div class="panel-heading"><strong>◈ OBJECT LIBRARY</strong><span>${state.objects.length} objects</span></div>${recent.length ? `<div class="object-grid" style="padding:9px">${recent.map(renderObjectCard).join('')}</div>` : `<div class="empty-state" style="margin:10px"><div class="empty-icon">🧰</div><h3>YOUR LIBRARY IS EMPTY</h3><p>Create an object type with the fields you care about, then add your first object.</p><button class="pixel-button gold" type="button" data-action="new-type">＋ Create Object Type</button></div>`}</section>
        <div style="display:grid;align-content:start;gap:10px">
          <section class="light-panel"><div class="panel-heading"><strong>▣ YOUR SECTIONS</strong><button class="pixel-button small" type="button" data-action="new-section">＋ Add</button></div>${state.sections.filter((section) => !section.archived).length ? `<div class="section-list">${state.sections.filter((section) => !section.archived).map((section) => `<button class="section-row" type="button" data-action="select-section" data-id="${esc(section.id)}"><span class="section-ic">${esc(section.icon)}</span><span>${esc(section.name)}</span><span class="section-count">${Number(section.object_count) || 0}</span></button>`).join('')}</div>` : `<div class="empty-state" style="margin:9px"><h3>MAKE YOUR OWN NAVIGATION</h3><p>Sections are fully customizable and can be nested.</p></div>`}</section>
          <section class="light-panel"><div class="panel-heading"><strong>◎ GOALS & PLANS</strong><button class="pixel-button small" type="button" data-action="open-goals">View all</button></div>${state.goals.length ? `<div class="goal-list">${state.goals.slice(0,3).map(renderGoal).join('')}</div>` : `<div class="empty-state" style="margin:9px"><h3>NO GOALS YET</h3><p>Track a manual milestone, checklist, or field target.</p><button class="pixel-button small" type="button" data-action="new-goal">＋ Create Goal</button></div>`}</section>
        </div>
      </div>`;
  }

  function renderSection() {
    const section = state.sections.find((item) => item.id === state.sectionId);
    if (!section) return renderOverview();
    const ids = new Set(state.objects.filter((object) => object.section_ids?.includes(section.id)).map((object) => object.id));
    const objects = state.objects.filter((object) => ids.has(object.id));
    const archived = state.sections.filter((item) => item.parent_id === section.id && !item.archived);
    return `<div class="content-heading"><div><h2>${esc(section.icon)} ${esc(section.name)}</h2><p>${objects.length} linked object${objects.length === 1 ? '' : 's'} · ${esc(section.display_style)} view</p></div><div class="toolbar"><button class="pixel-button gold" type="button" data-action="add-existing">＋ Add Existing</button><button class="pixel-button" type="button" data-action="new-object">＋ Create Object</button><button class="pixel-button soft" type="button" data-action="edit-section" data-id="${esc(section.id)}">✎</button><button class="pixel-button danger small" type="button" data-action="archive-section" data-id="${esc(section.id)}" title="Archive section">Archive</button></div></div>
      ${archived.length ? `<div class="section-list light-panel" style="margin-bottom:10px">${archived.map((child) => `<button class="section-row" type="button" data-action="select-section" data-id="${esc(child.id)}">${esc(child.icon)} ${esc(child.name)}<span class="section-count">Nested section</span></button>`).join('')}</div>` : ''}
      ${objects.length ? section.display_style==='list'?renderObjectList(objects):section.display_style==='table'?renderObjectTable(objects,section):section.display_style==='checklist'?renderChecklistView(objects,section):section.display_style==='progress'?renderProgressView(objects,section):`<div class="object-grid">${objects.map((object)=>renderObjectCard(object,section)).join('')}</div>` : `<div class="empty-state"><div class="empty-icon">🗂️</div><h3>THIS SECTION IS READY</h3><p>Add an existing object here or create a new one. The same object can live in several sections.</p><button class="pixel-button gold" type="button" data-action="add-existing">＋ Add Existing Object</button></div>`}`;
  }

  function renderObjectList(objects){
    return `<div class="object-list-view">${objects.map((object)=>`<article class="object-list-row light-panel"><div class="list-object-symbol">${object.image?`<img src="${esc(object.image)}" alt="">`:'◈'}</div><div class="list-object-main"><strong>${esc(object.name)}</strong><small>${esc(object.type_name)}</small><div class="list-object-values">${(object.fields||[]).filter((field)=>!field.archived).slice(0,2).map((field)=>`<span>${esc(field.icon||'✦')} ${esc(field.name)}: <b>${esc(prettyValue(object.values?.[field.id],field,state.objects))}</b></span>`).join('')}</div></div><div class="toolbar"><button class="pixel-button small" type="button" data-action="edit-object" data-id="${esc(object.id)}">Edit</button><button class="pixel-button small soft" type="button" data-action="remove-placement" data-id="${esc(object.id)}">Remove here</button></div></article>`).join('')}</div>`;
  }

  function renderObjectTable(objects,section){
    const config=sectionDisplayConfig(section);
    const configured=new Set(config.visible_field_ids||[]);
    const columns=[];for(const object of objects){for(const field of (object.fields||[]).filter((item)=>!item.archived)){if((configured.size===0||configured.has(field.id))&&!columns.some((item)=>item.id===field.id)&&columns.length<12)columns.push(field);}}
    return `<div class="table-scroll light-panel"><table class="object-table"><thead><tr><th>Object</th><th>Type</th>${columns.map((field)=>`<th>${esc(field.icon||'✦')} ${esc(field.name)}</th>`).join('')}<th>Actions</th></tr></thead><tbody>${objects.map((object)=>`<tr><td><strong>${esc(object.name)}</strong></td><td>${esc(object.type_name)}</td>${columns.map((field)=>`<td>${object.object_type_id===field.object_type_id?esc(prettyValue(object.values?.[field.id],field,state.objects)):'—'}</td>`).join('')}<td><button class="pixel-button small" type="button" data-action="edit-object" data-id="${esc(object.id)}">Edit</button> <button class="pixel-button small soft" type="button" data-action="remove-placement" data-id="${esc(object.id)}">Remove</button></td></tr>`).join('')}</tbody></table></div>`;
  }

  function renderChecklistView(objects,section){
    const fieldID=sectionDisplayConfig(section).checklist_field_id;
    const field=state.types.flatMap((type)=>type.fields||[]).find((item)=>item.id===fieldID&&!item.archived&&item.field_type==='boolean');
    if(!field)return `<div class="empty-state"><div class="empty-icon">☑</div><h3>CHOOSE A CHECKBOX FIELD</h3><p>Edit this section and select one of your custom checkbox fields to turn its objects into a checklist.</p><button class="pixel-button" type="button" data-action="edit-section" data-id="${esc(section.id)}">Configure checklist</button></div>`;
    const compatible=objects.filter((object)=>object.object_type_id===field.object_type_id);
    return `<section class="light-panel task-list section-checklist"><div class="panel-heading"><strong>${esc(field.icon||'☑')} ${esc(field.name)}</strong><span>${compatible.filter((object)=>Boolean(parseJSON(object.values?.[field.id]))).length} / ${compatible.length} complete</span></div>${compatible.map((object)=>`<div class="task-line"><input aria-label="${esc(object.name)}: ${esc(field.name)}" type="checkbox" data-action="toggle-object-checklist" data-object-id="${esc(object.id)}" data-field-id="${esc(field.id)}" ${parseJSON(object.values?.[field.id])?'checked':''}><span><strong>${esc(object.name)}</strong><small>${esc(object.type_name)}</small></span><button class="pixel-button small soft" type="button" data-action="edit-object" data-id="${esc(object.id)}">Edit</button></div>`).join('')||'<p class="empty-state">No objects of this field type are placed here yet.</p>'}</section>`;
  }

  function renderProgressView(objects,section){
    const fieldID=sectionDisplayConfig(section).progress_field_id;
    const field=state.types.flatMap((type)=>type.fields||[]).find((item)=>item.id===fieldID&&!item.archived&&item.field_type==='progress');
    if(!field)return `<div class="empty-state"><div class="empty-icon">▰</div><h3>CHOOSE A PROGRESS FIELD</h3><p>Edit this section and select a custom progress field. Each object can set its own current and maximum values.</p><button class="pixel-button" type="button" data-action="edit-section" data-id="${esc(section.id)}">Configure progress view</button></div>`;
    const compatible=objects.filter((object)=>object.object_type_id===field.object_type_id);
    return `<section class="light-panel section-progress-list"><div class="panel-heading"><strong>${esc(field.icon||'▰')} ${esc(field.name)}</strong><span>Current / maximum</span></div>${compatible.map((object)=>{const value=parseJSON(object.values?.[field.id])||{};const current=Number(value.current)||0;const maximum=Number(value.max)||0;const percent=maximum>0?Math.max(0,Math.min(100,current/maximum*100)):0;return `<article class="section-progress-row"><div><strong>${esc(object.name)}</strong><small>${maximum>0?`${esc(formatNumber(current))} / ${esc(formatNumber(maximum))}`:'No progress entered'}</small></div><div class="progress-track" role="progressbar" aria-valuenow="${Math.round(percent)}" aria-valuemin="0" aria-valuemax="100" aria-label="${esc(object.name)} progress"><div class="progress-fill" style="width:${percent}%"></div></div><b>${maximum>0?`${Math.round(percent)}%`:'—'}</b><button class="pixel-button small" type="button" data-action="edit-object" data-id="${esc(object.id)}">Edit</button></article>`;}).join('')||'<p class="empty-state">No compatible objects are placed here yet.</p>'}</section>`;
  }

  function renderObjectCard(object,section=null) {
    const config=section?sectionDisplayConfig(section):{};
    const selected=new Set(config.visible_field_ids||[]);
    const available=(object.fields||[]).filter((field)=>!field.archived);
    const fields = (selected.size?available.filter((field)=>selected.has(field.id)):available).slice(0, 6);
    const levelField = fields.find((field) => /level/i.test(field.name));
    const level = levelField ? prettyValue(object.values?.[levelField.id], levelField, state.objects) : '';
    const image = object.image ? `<img src="${esc(object.image)}" alt="">` : '';
    return `<article class="object-card">
      <div class="object-art">${image}<span class="object-art-symbol" ${object.image ? 'hidden' : ''}>${esc(object.type_name?.slice(0,1)?.toUpperCase() || '✦')}</span>${level ? `<span class="object-level">${esc(levelField.name)} ${esc(level)}</span>` : ''}</div>
      <div class="object-details"><div class="object-title-row"><div><h3 class="object-title">${esc(object.name)}</h3><div class="object-type-label">${esc(object.type_name)}</div></div></div>
        ${fields.length ? `<div class="object-fields">${fields.map((field) => {const error=object.formula_errors?.[field.id];return `<div class="field-pair"><span aria-hidden="true">${esc(field.icon || '✦')}</span><span class="field-label">${esc(field.name)}</span><span class="field-value ${error?'formula-error':''}" ${error?`title="${esc(error)}"`:''}>${error?`⚠ ${esc(error)}`:esc(prettyValue(object.values?.[field.id],field,state.objects))}</span></div>`;}).join('')}</div>` : `<p class="object-type-label">No fields defined yet</p>`}
        <div class="object-actions"><button class="pixel-button small" type="button" data-action="edit-object" data-id="${esc(object.id)}">✎ Edit</button>${state.panel === 'section' ? `<button class="pixel-button small soft" type="button" data-action="remove-placement" data-id="${esc(object.id)}">Remove here</button>` : ''}<button class="pixel-button small soft" type="button" data-action="archive-object" data-id="${esc(object.id)}">Archive</button></div>
      </div>
    </article>`;
  }

  function renderGoals() {
    return `<div class="content-heading"><div><h2>◎ GOALS & DEVELOPMENT PLANS</h2><p>Manual milestones, task checklists, numeric progress, and live field targets.</p></div><button class="pixel-button gold" type="button" data-action="new-goal">＋ Add Goal</button></div>
      ${state.goals.length ? `<div class="goal-list light-panel">${state.goals.map((goal) => renderGoal(goal,true)).join('')}</div>` : `<div class="empty-state"><div class="empty-icon">🏁</div><h3>EVERY QUEST STARTS SOMEWHERE</h3><p>Create a manual goal, a checklist of tasks, a numeric tracker, or a goal linked to one of your object fields.</p><button class="pixel-button gold" type="button" data-action="new-goal">＋ Create Your First Goal</button></div>`}`;
  }

  function renderGoal(goal, large = false) {
    const typeLabel = {manual:'Manual goal',checklist:'Checklist',field_target:'Linked field target',numeric:'Numeric progress'}[goal.goal_type] || 'Goal';
    const target = goal.target ? parseJSON(goal.target) : 0;
    const current = goal.current || 0;
    const statusText = goal.goal_type === 'field_target' || goal.goal_type === 'numeric' ? `${formatNumber(current)} / ${formatNumber(Number(target) || 0)}` : goal.goal_type === 'checklist' ? `${goal.tasks?.filter((task)=>task.completed).length || 0} / ${goal.tasks?.length || 0} tasks` : goal.completed ? 'Complete' : 'In progress';
    const width = Math.max(0,Math.min(100,Number(goal.progress)||0));
    let controls = '';
    if (goal.goal_type === 'manual') controls = `<button type="button" class="pixel-button small ${goal.completed?'soft':'gold'}" data-action="toggle-goal" data-id="${esc(goal.id)}">${goal.completed?'Mark active':'✓ Mark done'}</button>`;
    if (goal.goal_type === 'numeric') controls = `<button type="button" class="pixel-button small" data-action="edit-goal-progress" data-id="${esc(goal.id)}">Update progress</button>`;
    if (goal.goal_type === 'checklist' && large) controls = `<button type="button" class="pixel-button small" data-action="add-task" data-id="${esc(goal.id)}">＋ Task</button>`;
    return `<article class="goal-card"><div class="goal-top"><span aria-hidden="true">${goal.completed?'✅':goal.goal_type==='field_target'?'✧':goal.goal_type==='checklist'?'▣':'🏁'}</span><span class="goal-title">${esc(goal.title)}</span><span class="goal-state">${goal.completed?'✓':''}</span></div>
      ${goal.description ? `<p style="margin:4px 0;color:#705b70;font-size:12px">${esc(goal.description)}</p>` : ''}
      <div class="progress-meta"><span>${esc(typeLabel)}</span><span>${esc(statusText)} · ${Math.round(width)}%</span></div><div class="progress-track" role="progressbar" aria-valuenow="${Math.round(width)}" aria-valuemin="0" aria-valuemax="100" aria-label="${esc(goal.title)} progress"><div class="progress-fill ${goal.completed?'done':''}" style="width:${width}%"></div></div>
      ${goal.goal_type==='checklist' && large ? `<div class="task-list">${(goal.tasks||[]).map(task=>`<label class="task-line"><input type="checkbox" data-action="toggle-task" data-id="${esc(task.id)}" ${task.completed?'checked':''}><span>${esc(task.title)}</span></label>`).join('')}</div>` : ''}
      <div class="object-actions">${controls}<button type="button" class="pixel-button small soft" data-action="delete-goal" data-id="${esc(goal.id)}">Delete</button></div></article>`;
  }

  function renderSchema() {
    const types = state.types;
    return `<div class="content-heading"><div><h2>⚙ YOUR CUSTOM DATA TYPES</h2><p>Fields belong to a type and stay editable on each object. Their type cannot be changed after creation.</p></div><button class="pixel-button gold" type="button" data-action="new-type">＋ New Type</button></div>
      ${types.length ? `<div class="schema-grid">${types.map((type) => `<article class="schema-card"><h3><span>${esc(type.icon||'◈')}</span>${esc(type.name)}</h3><p>${esc(type.description || 'Custom objects in this game.')}</p><div class="field-editor-list">${(type.fields||[]).map((field) => `<div class="field-row ${field.archived?'field-archived':''}"><span class="field-icon">${esc(field.icon||'✦')}</span><strong>${esc(field.name)}</strong><span class="field-kind">${esc(typeLabel(field.field_type))}</span>${field.archived?`<button class="pixel-button small" type="button" data-action="restore-field" data-id="${esc(field.id)}">Restore</button>`:`<button class="pixel-button small soft" type="button" data-action="edit-field" data-id="${esc(field.id)}">Edit</button><button class="pixel-button small danger" type="button" data-action="archive-field" data-id="${esc(field.id)}" title="Archive field; values are preserved">Archive</button>`}</div>`).join('')}</div><div class="toolbar" style="margin-top:9px"><button class="pixel-button small" type="button" data-action="new-field" data-type="${esc(type.id)}">＋ Add Field</button></div></article>`).join('')}</div>` : `<div class="empty-state"><div class="empty-icon">🧩</div><h3>DESIGN YOUR OWN DATA</h3><p>Create a type and define characteristics that make sense for your game. No game comes with preset fields.</p><button class="pixel-button gold" type="button" data-action="new-type">＋ Create Object Type</button></div>`}`;
  }

  function renderWelcome() {
    return `<div class="game-hero"><div class="hero-symbol">✧</div><div class="hero-copy"><h1 class="hero-title">YOUR PERSONAL GAME TRACKER</h1><p class="hero-description">A cozy place for every build, quest, collection, and idea.</p></div></div><div class="content"><div class="empty-state" style="padding:48px 18px"><div class="empty-icon">🎮 ✦</div><h3>YOUR ADVENTURE BEGINS HERE</h3><p>Create your first game. Every section, object type, field, and goal after that is yours to define.</p><button class="pixel-button gold" type="button" data-action="new-game">＋ Add Your First Game</button></div></div>`;
  }

  function openForm(title, body, submit='SAVE') { openModal(ensureDialog(root),title,`<form id="modal-form" autocomplete="off">${body}</form>`,submit); }
  function fieldRow(label,name,value='',type='text',wide=false,extra='') { const icons=name==='icon'?` list="pixel-icon-options"`:'';const suggestions=name==='icon'?`<datalist id="pixel-icon-options"><option value="✦"><option value="❤"><option value="⚔"><option value="★"><option value="♛"><option value="◈"><option value="⌖"><option value="⚑"><option value="✿"><option value="☀"><option value="◎"><option value="🗺️"><option value="🎮"><option value="🛡️"></datalist>`:'';return `<div class="form-field ${wide?'wide':''}"><label for="f-${esc(name)}">${esc(label)}</label><input id="f-${esc(name)}" name="${esc(name)}" type="${type}" value="${esc(value??'')}"${icons} ${extra}>${suggestions}</div>`; }

  function openGameForm(edit=false) {
    const game=state.game||{};
    openForm(edit?'EDIT GAME':'ADD A GAME',`${fieldRow('Game name','name',game.name||'','text',true,'required maxlength="120"')}${fieldRow('Description','description',game.description||'','text',true,'maxlength="300"')}${fieldRow('Personal notes','personal_notes',game.personal_notes||'','text',true,'maxlength="2000"')}${fieldRow('Cover image URL (optional)','cover_image',game.cover_image||'','text',true)}<div class="form-field wide"><label for="f-cover-file">Or upload a cover image</label><input id="f-cover-file" name="cover_file" type="file" accept="image/png,image/jpeg,image/gif"><small>PNG, JPEG, or GIF · maximum 5 MB</small></div><input type="hidden" name="form_action" value="${edit?'edit-game':'create-game'}">`,edit?'SAVE GAME':'CREATE GAME');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form=edit?'edit-game':'create-game';
    if(edit){form.insertAdjacentHTML('beforeend',`<div class="form-field wide" style="margin-top:10px"><button class="pixel-button danger" type="button" data-action="archive-game">Archive this game</button><small>Archived data stays saved and can be restored in settings.</small></div>`);}
  }

  function openSectionForm(existing=null) {
    const sections=state.sections.filter((item)=>!item.archived&&item.id!==existing?.id);
    const parent=existing?.parent_id||'';
    const config=sectionDisplayConfig(existing||{});
    const allFields=state.types.flatMap((type)=>type.fields||[]).filter((field)=>!field.archived);
    const selectedFields=new Set(config.visible_field_ids||[]);
    const fieldChoices=allFields.length?allFields.map((field)=>`<label class="section-field-choice"><input type="checkbox" name="visible_field_ids" value="${esc(field.id)}" ${selectedFields.has(field.id)?'checked':''}><span>${esc(field.icon||'✦')} ${esc(field.name)}</span><small>${esc(field.type_name||state.types.find((type)=>type.id===field.object_type_id)?.name||'')}</small></label>`).join(''):'<small>Create custom fields to choose what this section displays.</small>';
    const checklistFields=allFields.filter((field)=>field.field_type==='boolean');
    const progressFields=allFields.filter((field)=>field.field_type==='progress');
    const selectedOption=(fieldID,placeholder,fields)=>`<option value="">${placeholder}</option>${fields.map((field)=>`<option value="${esc(field.id)}" ${field.id===fieldID?'selected':''}>${esc(state.types.find((type)=>type.id===field.object_type_id)?.name||'')} · ${esc(field.name)}</option>`).join('')}`;
    const body=`${fieldRow('Section name','name',existing?.name||'','text',true,'required maxlength="80"')}${fieldRow('Icon or emoji','icon',existing?.icon||'✦','text',false,'maxlength="8"')}<div class="form-field"><label for="f-display_style">Display style</label><select id="f-display_style" name="display_style"><option value="cards" ${existing?.display_style==='cards'?'selected':''}>Cards</option><option value="list" ${existing?.display_style==='list'?'selected':''}>List</option><option value="table" ${existing?.display_style==='table'?'selected':''}>Table</option><option value="checklist" ${existing?.display_style==='checklist'?'selected':''}>Checklist</option><option value="progress" ${existing?.display_style==='progress'?'selected':''}>Progress</option></select></div><div class="form-field wide"><label for="f-parent_id">Parent section</label><select id="f-parent_id" name="parent_id"><option value="">Top level</option>${sections.map((item)=>`<option value="${esc(item.id)}" ${item.id===parent?'selected':''}>${esc(item.name)}</option>`).join('')}</select><small>Sections can be nested as deeply as you like.</small></div><div class="form-field wide"><label>Visible fields for cards and tables</label><div class="section-field-choices">${fieldChoices}</div><small>Leave all unchecked to use the default fields in each view.</small></div><div class="form-field"><label for="f-checklist_field_id">Checklist checkbox field</label><select name="checklist_field_id" id="f-checklist_field_id">${selectedOption(config.checklist_field_id,'Choose a checkbox field',checklistFields)}</select></div><div class="form-field"><label for="f-progress_field_id">Progress field</label><select name="progress_field_id" id="f-progress_field_id">${selectedOption(config.progress_field_id,'Choose a progress field',progressFields)}</select></div>${existing?fieldRow('Sort position','sort_order',existing.sort_order,'number',false,'step="1"'):''}<input type="hidden" name="form_action" value="${existing?'edit-section':'create-section'}">${existing?`<input type="hidden" name="section_id" value="${esc(existing.id)}">`:''}`;
    openForm(existing?'EDIT SECTION':'CREATE A SECTION',body,existing?'SAVE SECTION':'CREATE SECTION');ensureDialog(root).querySelector('#modal-form').dataset.form=existing?'edit-section':'create-section';
  }

  function openTypeForm() {
    openForm('CREATE AN OBJECT TYPE',`${fieldRow('Type name','name','','text',true,'required maxlength="80"')}${fieldRow('Icon or emoji','icon','◈','text')}${fieldRow('Description','description','','text',true,'maxlength="300"')}<p class="dialog-help wide">Type names are labels only. The app does not assume what any game type contains.</p>`, 'CREATE TYPE');ensureDialog(root).querySelector('#modal-form').dataset.form='create-type';
  }

  function openFieldForm(typeID,existing=null) {
    const type=state.types.find((item)=>item.id===(typeID||existing?.object_type_id));if(!type)return;
    const selectedType=existing?.field_type||'text';
    const options=parseJSON(existing?.options)||{};
    const defaultValue=existing?.default_value===undefined?'':typeof existing.default_value==='string'?existing.default_value:JSON.stringify(existing.default_value);
    let body=`${fieldRow('Field name','name',existing?.name||'','text',true,'required maxlength="80"')}${fieldRow('Icon or emoji','icon',existing?.icon||'✦','text')}`;
    body+=`<div class="form-field wide"><label for="f-field_type">Field type ${existing?'(locked after creation)':''}</label><select id="f-field_type" name="field_type" ${existing?'disabled':''}>${FIELD_TYPES.map(([id,label])=>`<option value="${id}" ${selectedType===id?'selected':''}>${label}</option>`).join('')}</select></div>`;
    body+=fieldRow('Description','description',existing?.description||'','text',true,'maxlength="300"');
    body+=`<div class="form-field wide" id="field-kind-options"></div>`;
    body+=`<div class="form-field wide" id="default-field">${fieldRow('Default value (optional)','default_value',defaultValue,'text',true,'maxlength="1000"')}</div>`;
    if(existing)body+=`<input type="hidden" name="field_type" value="${esc(existing.field_type)}"><input type="hidden" name="field_id" value="${esc(existing.id)}"><input type="hidden" name="object_type_id" value="${esc(existing.object_type_id)}">`;
    else body+=`<input type="hidden" name="object_type_id" value="${esc(type.id)}">`;
    body+=`<p class="dialog-help wide">The field type is permanent once created. You can rename, reorder, and archive the field later; archived values remain saved.</p>`;
    openForm(existing?'EDIT FIELD':'ADD A FIELD',body,existing?'SAVE FIELD':'CREATE FIELD');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form=existing?'edit-field':'create-field';
    const typeSelect=form.querySelector('#f-field_type');
    const renderOptions=()=>{form.querySelector('#field-kind-options').innerHTML=fieldOptionsHTML(typeSelect.value,options,type.fields||[]);form.querySelector('#default-field').hidden=['formula','reference','references'].includes(typeSelect.value);};
    typeSelect.addEventListener('change',renderOptions);renderOptions();
  }

  function fieldOptionsHTML(fieldType,options={},typeFields=[]) {
    if(fieldType==='select')return `<label for="f-choices">Choices (one per line)</label><textarea id="f-choices" name="choices" required>${esc((options.choices||[]).join('\n'))}</textarea>`;
    if(fieldType==='reference'||fieldType==='references')return `<label for="f-allowed_type_id">Allowed target type</label><select id="f-allowed_type_id" name="allowed_type_id"><option value="">Any type in this game</option>${state.types.map((item)=>`<option value="${esc(item.id)}" ${options.allowed_type_id===item.id?'selected':''}>${esc(item.name)}</option>`).join('')}</select>`;
    if(fieldType==='formula')return `<label for="f-expression">Formula expression</label><input id="f-expression" name="expression" value="${esc(options.expression||'')}" placeholder="@{field-id} + 12" required><small>Use numeric fields from this type as @{stable-field-id}. Allowed: + − × ÷ and parentheses. Example: (@{${esc(typeFields.find((field)=>['integer','decimal','percentage'].includes(field.field_type))?.id||'field-id')}} + 5) / 2</small>`;
    if(fieldType==='progress')return `<small>Enter the current and maximum values on each object. A percentage is calculated for display.</small>`;
    return '';
  }

  function openObjectForm(object=null) {
    const type=object?state.types.find((item)=>item.id===object.object_type_id):state.types.find((item)=>item.id===root.querySelector('[data-selected-type]')?.dataset.selectedType)||state.types[0];
    if(!type){openTypeForm();toast('Create an object type first, then add objects.',true);return;}
    const activeFields=(type.fields||[]).filter((field)=>!field.archived);
    const imageInput=`<div class="form-field wide"><label for="f-object-file">Object image (optional)</label><input id="f-object-file" name="image_file" type="file" accept="image/png,image/jpeg,image/gif"><small>PNG, JPEG, or GIF · maximum 5 MB</small></div>`;
    const fields=activeFields.map((field)=>renderFieldInput(field,object?.values?.[field.id],object)).join('');
    const typeSelect=object?'':`<div class="form-field wide"><label for="f-object_type_id">Object type</label><select id="f-object_type_id" name="object_type_id" data-action="choose-object-type">${state.types.map((item)=>`<option value="${esc(item.id)}" ${item.id===type.id?'selected':''}>${esc(item.name)}</option>`).join('')}</select></div><div class="form-field wide" id="object-field-inputs">${fields}</div>`;
    const simple=`${fieldRow('Object name','name',object?.name||'','text',true,'required maxlength="160"')}${fieldRow('Description','description',object?.description||'','text',true,'maxlength="500"')}`;
    const sectionSelector=!object&&state.panel==='section'?`<div class="form-field wide"><label for="f-section_id">Also place in section</label><select name="section_id" id="f-section_id"><option value="">Do not place in a section</option>${state.sections.filter((item)=>!item.archived).map((item)=>`<option value="${esc(item.id)}" ${item.id===state.sectionId?'selected':''}>${esc(item.name)}</option>`).join('')}</select></div>`:'';
    const formAction=object?'edit-object':'create-object';
    openForm(object?'EDIT OBJECT':'CREATE AN OBJECT',`${simple}${typeSelect}${object?`<div class="form-field wide">Type: <strong>${esc(type.name)}</strong></div>`:''}<div class="form-field wide" id="object-fields">${object?fields:''}</div>${imageInput}${sectionSelector}<input type="hidden" name="type_id" value="${esc(type.id)}">${object?`<input type="hidden" name="object_id" value="${esc(object.id)}">`:''}<p class="dialog-help wide">Objects belong to the game. You can place the same object in several sections.</p>`,object?'SAVE OBJECT':'CREATE OBJECT');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form=formAction;form.dataset.typeId=type.id;
  }

  function renderFieldInput(field,rawValue,object) {
    const value=parseJSON(rawValue);
    const name=`${field.id}`;
    const label=`${field.icon||'✦'} ${field.name}`;
    if(field.field_type==='formula')return `<div class="form-field wide"><label>${esc(label)} · calculated</label><input readonly value="${esc(object?.formula_errors?.[field.id]||(value??'Calculated when inputs are filled'))}"><small>${esc(object?.formula_errors?.[field.id]||'This value comes from your formula configuration.')}</small></div>`;
    if(field.field_type==='boolean')return `<div class="form-field"><label for="f-${esc(name)}">${esc(label)}</label><input id="f-${esc(name)}" name="${esc(name)}" type="checkbox" ${value?'checked':''}></div>`;
    if(field.field_type==='long_text')return `<div class="form-field wide"><label for="f-${esc(name)}">${esc(label)}</label><textarea id="f-${esc(name)}" name="${esc(name)}" maxlength="50000">${esc(value??'')}</textarea></div>`;
    if(field.field_type==='select'){const options=parseJSON(field.options)||{};return `<div class="form-field"><label for="f-${esc(name)}">${esc(label)}</label><select id="f-${esc(name)}" name="${esc(name)}"><option value="">Choose…</option>${(options.choices||[]).map((choice)=>`<option value="${esc(choice)}" ${choice===value?'selected':''}>${esc(choice)}</option>`).join('')}</select></div>`;}
    if(field.field_type==='reference'||field.field_type==='references'){const options=parseJSON(field.options)||{};const targets=state.objects.filter((item)=>item.id!==object?.id&&(!options.allowed_type_id||item.object_type_id===options.allowed_type_id));const selected=new Set(Array.isArray(value)?value:[value].filter(Boolean));return `<div class="form-field ${field.field_type==='references'?'wide':''}"><label for="f-${esc(name)}">${esc(label)}</label><select id="f-${esc(name)}" name="${esc(name)}" ${field.field_type==='references'?'multiple':''} ${field.field_type==='references'?'size="4"':''}><option value="">${field.field_type==='references'?'Select one or more…':'No reference'}</option>${targets.map((target)=>`<option value="${esc(target.id)}" ${selected.has(target.id)?'selected':''}>${esc(target.name)} · ${esc(target.type_name)}</option>`).join('')}</select></div>`;}
    if(field.field_type==='progress'){const current=value?.current??'';const max=value?.max??'';return `<div class="form-field"><label>${esc(label)}</label><div style="display:flex;gap:6px"><input aria-label="Current" type="number" step="any" name="${esc(name)}__current" data-progress-field="${esc(name)}" value="${esc(current)}" placeholder="Current"><input aria-label="Maximum" type="number" step="any" name="${esc(name)}__max" data-progress-field="${esc(name)}" value="${esc(max)}" placeholder="Maximum"></div></div>`;}
    const htmlType=field.field_type==='date'?'date':field.field_type==='integer'?'number':field.field_type==='decimal'||field.field_type==='percentage'?'number':'text';
    const step=field.field_type==='integer'?'1':field.field_type==='decimal'||field.field_type==='percentage'?'any':null;
    const inputType=htmlType==='number'?`type="number" step="${step}"`: `type="${htmlType}"`;
    return `<div class="form-field"><label for="f-${esc(name)}">${esc(label)}</label><input id="f-${esc(name)}" name="${esc(name)}" ${inputType} value="${esc(value??'')}" ${field.field_type==='percentage'?'min="0" max="100"':''}></div>`;
  }

  function readValues(form, type) {
    const values={};
    for(const field of (type.fields||[]).filter((item)=>!item.archived)){
      if(field.field_type==='formula')continue;
      const el=form.elements.namedItem(field.id);
      if(field.field_type==='boolean'){values[field.id]=Boolean(el?.checked);continue;}
      if(field.field_type==='reference'||field.field_type==='references'){
        const selected=[...(el?.selectedOptions||[])].map((option)=>option.value).filter(Boolean);
        values[field.id]=field.field_type==='reference'?(selected[0]||null):selected;continue;
      }
      if(field.field_type==='progress'){
        const current=form.elements.namedItem(`${field.id}__current`)?.value;
        const max=form.elements.namedItem(`${field.id}__max`)?.value;
        values[field.id]=current===''&&max===''?null:{current:Number(current||0),max:Number(max||0)};continue;
      }
      let value=el?.value??'';
      if(value===''){values[field.id]=null;continue;}
      if(['integer','decimal','percentage'].includes(field.field_type))value=Number(value);
      values[field.id]=value;
    }
    return values;
  }

  function openAddExisting() {
    const already=new Set(state.objects.filter((object)=>object.section_ids?.includes(state.sectionId)).map((object)=>object.id));
    const available=state.objects.filter((object)=>!already.has(object.id));
    if(!available.length){toast('Every active object is already in this section.',true);return;}
    openForm('ADD EXISTING OBJECT',`<p class="dialog-help">Choose an object to place here. Its values remain shared with every section.</p><div class="form-field wide"><label for="f-object_id">Object</label><select id="f-object_id" name="object_id" required>${available.map((item)=>`<option value="${esc(item.id)}">${esc(item.name)} · ${esc(item.type_name)}</option>`).join('')}</select></div>`, 'ADD TO SECTION');ensureDialog(root).querySelector('#modal-form').dataset.form='add-placement';
  }

  function openGoalForm() {
    const typeOptions=`<option value="manual">Manual goal</option><option value="checklist">Checklist</option><option value="numeric">Numeric progress</option><option value="field_target">Target one of my object fields</option>`;
    const body=`${fieldRow('Goal title','title','','text',true,'required maxlength="160"')}${fieldRow('Description (optional)','description','','text',true,'maxlength="500"')}<div class="form-field wide"><label for="f-goal_type">Goal style</label><select id="f-goal_type" name="goal_type" data-action="choose-goal-type">${typeOptions}</select></div><div class="form-field wide" id="goal-specific-fields"><p class="dialog-help">A manual goal can be checked off when you finish it.</p></div>`;
    openForm('CREATE A GOAL',body,'CREATE GOAL');const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form='create-goal';renderGoalSpecific(form,'manual');
  }

  function renderGoalSpecific(form,goalType) {
    const target=form.querySelector('#goal-specific-fields');if(!target)return;
    if(goalType==='checklist')target.innerHTML=`<div class="form-field"><label for="f-tasks">Checklist tasks</label><textarea id="f-tasks" name="tasks" placeholder="One task per line" required></textarea><small>Add one task per line.</small></div>`;
    else if(goalType==='numeric')target.innerHTML=`${fieldRow('Starting progress','current','0','number')}${fieldRow('Target','target','100','number',false,'required min="0.000001" step="any"')}`;
    else if(goalType==='field_target'){
      const choices=[];for(const object of state.objects){for(const field of (object.fields||[])){if(['integer','decimal','percentage','progress','formula'].includes(field.field_type)&&!field.archived)choices.push({object,field});}}
      target.innerHTML=`<div class="form-field"><label for="f-target_object">Object</label><select name="object_id" id="f-target_object" required>${choices.map(({object})=>`<option value="${esc(object.id)}">${esc(object.name)} · ${esc(object.type_name)}</option>`).filter((html,index,array)=>array.findIndex((x)=>x===html)===index).join('')}</select></div><div class="form-field"><label for="f-target_field">Field</label><select name="field_id" id="f-target_field" required>${choices.map(({object,field})=>`<option value="${esc(field.id)}" data-object="${esc(object.id)}">${esc(object.name)} · ${esc(field.name)}</option>`).join('')}</select></div>${fieldRow('Target value','target','','number',false,'required step="any"')}<small class="dialog-help wide">Progress updates automatically when the object field changes.</small>`;
      target.querySelector('#f-target_object')?.addEventListener('change',(event)=>{const objectID=event.target.value;const select=target.querySelector('#f-target_field');for(const option of select.options)option.hidden=option.dataset.object!==objectID;const first=[...select.options].find((option)=>!option.hidden);if(first)select.value=first.value;});target.querySelector('#f-target_object')?.dispatchEvent(new Event('change'));
    } else target.innerHTML=`<p class="dialog-help">A manual goal can be checked off when you finish it.</p>`;
  }

  function openAddTask(goal) { openForm('ADD A CHECKLIST TASK',`${fieldRow('Task name','title','','text',true,'required maxlength="160"')}<input type="hidden" name="goal_id" value="${esc(goal.id)}">`,'ADD TASK');ensureDialog(root).querySelector('#modal-form').dataset.form='add-task'; }

  async function openProfile() {
    const allGames=await getItems('/api/games?archived=true');
    const archivedGames=allGames.filter((item)=>item.archived);
    const archivedObjects=state.game?await getItems(`/api/games/${state.game.id}/objects?archived=true`):[];
    const archivedSections=state.sections.filter((item)=>item.archived);
    const archivedItems=[
      ...archivedGames.map((item)=>`<div class="section-row"><span>🎮</span><span>${esc(item.name)}</span><button type="button" class="pixel-button small" data-action="restore-game" data-id="${esc(item.id)}">Restore</button></div>`),
      ...archivedSections.map((item)=>`<div class="section-row"><span>${esc(item.icon)}</span><span>${esc(item.name)} <small>section</small></span><button type="button" class="pixel-button small" data-action="restore-section" data-id="${esc(item.id)}">Restore</button></div>`),
      ...archivedObjects.filter((item)=>item.archived).map((item)=>`<div class="section-row"><span>◈</span><span>${esc(item.name)} <small>${esc(item.type_name)}</small></span><button type="button" class="pixel-button small" data-action="restore-object" data-id="${esc(item.id)}">Restore</button></div>`),
    ];
    const archives=archivedItems.length?`<div class="form-field wide"><label>Archived data</label><div class="section-list light-panel">${archivedItems.join('')}</div></div>`:`<p class="dialog-help wide">There is no archived data to restore right now.</p>`;
    openForm('PROFILE & SETTINGS',`<p><strong>Username:</strong> ${esc(state.user.username)}</p><p class="dialog-help">Your account and game data are stored in the local SQLite database.</p><div class="form-field wide"><label>Change password</label></div>${fieldRow('Current password','current_password','','password',true,'required autocomplete="current-password"')}${fieldRow('New password','password','','password',true,'required minlength="10" maxlength="72" autocomplete="new-password"')}${fieldRow('Confirm new password','confirm_password','','password',true,'required minlength="10" maxlength="72" autocomplete="new-password"')}${archives}`, 'UPDATE PASSWORD');
    ensureDialog(root).querySelector('#modal-form').dataset.form='change-password';
  }

  function toast(message,isError=false) {
    const region=document.querySelector('#toast-region');if(!region)return;const item=document.createElement('div');item.className=`toast${isError?' error':''}`;item.textContent=message;region.append(item);setTimeout(()=>item.remove(),3600);
  }

  async function run(action) {
    try { await action(); } catch(error) { toast(error.message,true); }
  }

  async function submitForm(form) {
    const data=new FormData(form);const action=form.dataset.form;const values=Object.fromEntries(data.entries());
    if(action==='create-game'||action==='edit-game'){
      let cover=values.cover_image||'';const file=data.get('cover_file');if(file?.size){cover=await uploadImage(file,null);}
      const payload={name:values.name,description:values.description,personal_notes:values.personal_notes,cover_image:cover};
      if(action==='create-game'){const created=await request('/api/games',{method:'POST',body:payload});state.game=created;state.panel='overview';await load();}
      else{await request(`/api/games/${state.game.id}`,{method:'PATCH',body:payload});await load();}
    } else if(action==='create-section'){
      const created=await request(`/api/games/${state.game.id}/sections`,{method:'POST',body:{name:values.name,icon:values.icon,parent_id:values.parent_id||null,display_style:values.display_style,display_config:readSectionDisplayConfig(form)}});await load();state.panel='section';state.sectionId=created.id;render();
    } else if(action==='edit-section'){
      await request(`/api/sections/${values.section_id}`,{method:'PATCH',body:{name:values.name,icon:values.icon,parent_id:values.parent_id||null,display_style:values.display_style,display_config:readSectionDisplayConfig(form),sort_order:Number(values.sort_order||0)}});await load();
    } else if(action==='create-type'){
      await request(`/api/games/${state.game.id}/object-types`,{method:'POST',body:{name:values.name,icon:values.icon,description:values.description}});await load();state.panel='schema';render();
    } else if(action==='create-field'||action==='edit-field'){
      const fieldType=values.field_type;let options={};if(fieldType==='select')options.choices=(values.choices||'').split('\n').map((item)=>item.trim()).filter(Boolean);if(fieldType==='reference'||fieldType==='references'){if(values.allowed_type_id)options.allowed_type_id=values.allowed_type_id;}if(fieldType==='formula')options.expression=values.expression;
      const payload={name:values.name,field_type:fieldType,icon:values.icon,description:values.description,options};if(values.default_value!=='')payload.default_value=readDefaultValue(values.default_value,fieldType);
      if(action==='create-field')await request(`/api/object-types/${values.object_type_id}/fields`,{method:'POST',body:{...payload,sort_order:values.sort_order?Number(values.sort_order):undefined}});
      else await request(`/api/fields/${values.field_id}`,{method:'PATCH',body:{...payload,field_type:fieldType}});
      await load();state.panel='schema';render();
    } else if(action==='create-object'){
      const type=state.types.find((item)=>item.id===values.object_type_id);const file=data.get('image_file');const image=file?.size?await uploadImage(file,state.game.id):'';const payload={object_type_id:values.object_type_id,name:values.name,description:values.description,image,section_id:values.section_id||'',values:readValues(form,type)};await request(`/api/games/${state.game.id}/objects`,{method:'POST',body:payload});await load();
    } else if(action==='edit-object'){
      const object=state.objects.find((item)=>item.id===values.object_id);const type=state.types.find((item)=>item.id===object.object_type_id);const file=data.get('image_file');const image=file?.size?await uploadImage(file,state.game.id):object.image;await request(`/api/objects/${object.id}`,{method:'PATCH',body:{name:values.name,description:values.description,image}});await request(`/api/objects/${object.id}/values`,{method:'PATCH',body:{values:readValues(form,type)}});await load();
    } else if(action==='add-placement'){
      await request(`/api/sections/${state.sectionId}/placements`,{method:'POST',body:{object_id:values.object_id}});await load();
    } else if(action==='create-goal'){
      const goalType=values.goal_type;const payload={title:values.title,description:values.description,goal_type:goalType};if(goalType==='checklist')payload.tasks=(values.tasks||'').split('\n').map((item)=>item.trim()).filter(Boolean);if(goalType==='numeric'){payload.current=Number(values.current||0);payload.target=Number(values.target);}if(goalType==='field_target'){payload.object_id=values.object_id;payload.field_id=values.field_id;payload.target=Number(values.target);}await request(`/api/games/${state.game.id}/goals`,{method:'POST',body:payload});await load();state.panel='goals';render();
    } else if(action==='edit-goal-progress'){
      const goal=state.goals.find((item)=>item.id===values.goal_id);await request(`/api/goals/${goal.id}`,{method:'PATCH',body:{current:Number(values.current)}});await load();
    } else if(action==='add-task'){
      await request(`/api/goals/${values.goal_id}/tasks`,{method:'POST',body:{title:values.title}});await load();
    } else if(action==='change-password'){
      await request('/api/auth/password',{method:'POST',body:{current_password:values.current_password,password:values.password,confirm_password:values.confirm_password}});toast('Password updated.');
    }
    closeModal(ensureDialog(root));toast('Saved successfully.');
  }

  async function uploadImage(file,gameID) {
    if(file.size>5*1024*1024)throw new Error('Image must be smaller than 5 MB.');const form=new FormData();form.append('file',file);if(gameID)form.append('game_id',gameID);const uploaded=await request('/api/assets',{method:'POST',body:form});return uploaded.url;
  }

  root.addEventListener('input',(event)=>{if(event.target.id==='game-search'){state.search=event.target.value;const list=root.querySelector('.game-list');if(list)list.innerHTML=renderGameList();}});
  root.addEventListener('change',(event)=>{
    const action=event.target.dataset.action;
    if(action==='choose-object-type'){const type=state.types.find((item)=>item.id===event.target.value);const form=event.target.closest('form');form.dataset.typeId=type.id;form.querySelector('input[name=type_id]').value=type.id;const fields=form.querySelector('#object-field-inputs');if(fields)fields.innerHTML=(type.fields||[]).filter((field)=>!field.archived).map((field)=>renderFieldInput(field,null,null)).join('');}
    if(action==='choose-goal-type'){renderGoalSpecific(event.target.closest('form'),event.target.value);}
    if(action==='toggle-task'){run(async()=>{await request(`/api/goal-tasks/${event.target.dataset.id}`,{method:'PATCH',body:{completed:event.target.checked}});await load();});}
    if(action==='toggle-object-checklist'){const object=state.objects.find((item)=>item.id===event.target.dataset.objectId);const fieldID=event.target.dataset.fieldId;run(async()=>{await request(`/api/objects/${object.id}/values`,{method:'PATCH',body:{values:{[fieldID]:event.target.checked}}});await load();});}
  });
  root.addEventListener('click',(event)=>{
    const button=event.target.closest('[data-action]');if(!button)return;const action=button.dataset.action;const id=button.dataset.id;
    if(action==='new-game')openGameForm(false);
    else if(action==='edit-game')openGameForm(true);
    else if(action==='select-game'){state.game=state.games.find((item)=>item.id===id);state.panel='overview';state.sectionId=null;run(load);}
    else if(action==='select-panel'){state.panel=button.dataset.panel;state.sectionId=null;render();}
    else if(action==='select-section'){state.panel='section';state.sectionId=id;render();}
    else if(action==='new-section')openSectionForm();
    else if(action==='edit-section')openSectionForm(state.sections.find((item)=>item.id===id));
    else if(action==='archive-section')run(async()=>{if(!confirm('Archive this section? Its objects will remain saved and can still appear in other sections.'))return;await request(`/api/sections/${id}`,{method:'DELETE'});state.sectionId=null;state.panel='overview';await load();toast('Section archived.');});
    else if(action==='new-type')openTypeForm();
    else if(action==='new-field')openFieldForm(button.dataset.type);
    else if(action==='edit-field'){const field=state.types.flatMap((item)=>item.fields||[]).find((item)=>item.id===id);openFieldForm(field?.object_type_id,field);}
    else if(action==='archive-field')run(async()=>{if(!confirm('Archive this field? Existing values will be preserved.'))return;await request(`/api/fields/${id}/archive`,{method:'POST',body:{}});await load();toast('Field archived.');});
    else if(action==='restore-field')run(async()=>{await request(`/api/fields/${id}/restore`,{method:'POST',body:{}});await load();toast('Field restored.');});
    else if(action==='new-object')openObjectForm();
    else if(action==='edit-object')openObjectForm(state.objects.find((item)=>item.id===id));
    else if(action==='archive-object')run(async()=>{if(!confirm('Archive this object? It will be hidden from normal views and can be restored later.'))return;await request(`/api/objects/${id}/archive`,{method:'POST',body:{}});await load();toast('Object archived.');});
    else if(action==='add-existing')openAddExisting();
    else if(action==='remove-placement')run(async()=>{if(!confirm('Remove this object from the current section? The object itself stays in the game.'))return;await request(`/api/sections/${state.sectionId}/placements/${id}`,{method:'DELETE'});await load();toast('Object removed from this section.');});
    else if(action==='new-goal')openGoalForm();
    else if(action==='open-goals'){state.panel='goals';render();}
    else if(action==='toggle-goal')run(async()=>{const goal=state.goals.find((item)=>item.id===id);await request(`/api/goals/${id}`,{method:'PATCH',body:{completed:!goal.completed}});await load();});
    else if(action==='edit-goal-progress'){const goal=state.goals.find((item)=>item.id===id);openForm('UPDATE GOAL PROGRESS',`${fieldRow('Current progress','current',goal.current,'number',true,'min="0" step="any" required')}<input type="hidden" name="goal_id" value="${esc(id)}">`,'UPDATE');ensureDialog(root).querySelector('#modal-form').dataset.form='edit-goal-progress';}
    else if(action==='delete-goal')run(async()=>{if(!confirm('Delete this goal and its checklist tasks?'))return;await request(`/api/goals/${id}`,{method:'DELETE'});await load();toast('Goal deleted.');});
    else if(action==='add-task'){const goal=state.goals.find((item)=>item.id===id);openAddTask(goal);}
    else if(action==='archive-game')run(async()=>{if(!confirm(`Archive ${state.game.name}? Your data remains saved.`))return;await request(`/api/games/${state.game.id}/archive`,{method:'POST',body:{}});await load();closeModal(ensureDialog(root));toast('Game archived.');});
    else if(action==='restore-game')run(async()=>{await request(`/api/games/${id}/restore`,{method:'POST',body:{}});await load();await openProfile();toast('Game restored.');});
    else if(action==='restore-section')run(async()=>{await request(`/api/sections/${id}/restore`,{method:'POST',body:{}});await load();await openProfile();toast('Section restored.');});
    else if(action==='restore-object')run(async()=>{await request(`/api/objects/${id}/restore`,{method:'POST',body:{}});await load();await openProfile();toast('Object restored.');});
    else if(action==='logout')run(async()=>{await request('/api/auth/logout',{method:'POST'});setCSRF('');window.location.reload();});
    else if(action==='profile')run(openProfile);
  });
  root.addEventListener('submit',(event)=>{if(event.target.id!=='modal-form')return;event.preventDefault();const submit=event.target.querySelector('button[type=submit]');if(submit){submit.disabled=true;submit.textContent='SAVING…';}run(()=>submitForm(event.target)).catch(()=>{}).finally(()=>{if(submit?.isConnected){submit.disabled=false;submit.textContent='SAVE';}});});

  load().catch((error)=>{root.innerHTML=`<main class="auth-screen"><section class="auth-card"><div class="auth-inner"><h1 class="auth-title">COULD NOT OPEN THE LIBRARY</h1><p class="auth-copy">${esc(error.message)}</p><button class="pixel-button gold" type="button" onclick="location.reload()">TRY AGAIN</button></div></section></main>`;});
}

function sectionDepth(section,sections){let depth=0;let parent=section.parent_id;let guard=0;while(parent&&guard++<20){depth++;parent=sections.find((item)=>item.id===parent)?.parent_id;}return depth;}
function sectionDisplayConfig(section){const config=parseJSON(section?.display_config)||{};return config&&typeof config==='object'?config:{};}
function readSectionDisplayConfig(form){return {visible_field_ids:[...form.querySelectorAll('input[name="visible_field_ids"]:checked')].map((input)=>input.value),checklist_field_id:form.elements.namedItem('checklist_field_id')?.value||'',progress_field_id:form.elements.namedItem('progress_field_id')?.value||''};}
function typeLabel(type){return FIELD_TYPES.find(([id])=>id===type)?.[1]||type;}
function prettyValue(raw,field,objects){
  const value=parseJSON(raw);if(value===undefined||value===null||value==='')return '—';
  if(typeof value==='boolean')return value?'Yes':'No';
  if(field.field_type==='progress'&&value&&typeof value==='object'){const current=Number(value.current)||0;const max=Number(value.max)||0;return max?`${formatNumber(current)} / ${formatNumber(max)} (${Math.round(current/max*100)}%)`:'—';}
  if(field.field_type==='reference'||field.field_type==='references'){const ids=Array.isArray(value)?value:[value];const labels=ids.map((id)=>objects.find((item)=>item.id===id)?.name||String(id).slice(0,8));return labels.join(', ')||'—';}
  if(field.field_type==='percentage')return `${formatNumber(value)}%`;
  return typeof value==='object'?JSON.stringify(value):String(value);
}
function parseJSON(value){if(value===undefined||value===null)return undefined;if(typeof value!=='string')return value;try{return JSON.parse(value);}catch{return value;}}
function formatNumber(value){const number=Number(value);return Number.isFinite(number)?new Intl.NumberFormat(undefined,{maximumFractionDigits:2}).format(number):'0';}
function readDefaultValue(raw,fieldType){
  if(['integer','decimal','percentage'].includes(fieldType)){const value=Number(raw);if(!Number.isFinite(value))throw new Error('Default value must be a valid number.');return value;}
  if(fieldType==='boolean'){if(raw!=='true'&&raw!=='false')throw new Error('Boolean default must be true or false.');return raw==='true';}
  if(fieldType==='progress'){try{return JSON.parse(raw);}catch{throw new Error('Progress default must be JSON with current and max values.');}}
  if(fieldType==='select'||fieldType==='text'||fieldType==='long_text'||fieldType==='date')return raw;
  throw new Error('This field type cannot have a default value.');
}
function esc(value){return String(value??'').replace(/[&<>"']/g,(char)=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));}
