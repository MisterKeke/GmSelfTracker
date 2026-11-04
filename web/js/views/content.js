import { typeLabel, esc, renderIcon } from '../utils/formatting.js';

export function createContentViews({ renderObjectControls, renderObjectList, renderObjectTable, renderChecklistView, renderProgressView, renderObjectCard, renderGoals, renderGoal, state, root }) {
  function renderPanel() {
    if (state.panel === 'library') return renderLibrary();
    if (state.panel === 'goals') return renderGoals();
    if (state.panel === 'schema') return renderSchema();
    if (state.panel === 'section') return renderSection();
    return renderOverview();
  }

  function renderOverview() {
    const recent = [...state.objects].slice(0, 6);
    return `<div class="content-heading"><div><h2>✦ YOUR GAME SPACE</h2><p>Your data model is yours to shape. Start with sections, types, or objects.</p></div><div class="toolbar"><button class="pixel-button gold" type="button" data-action="new-object">＋ New Object</button><button class="pixel-button" type="button" data-action="new-goal">◎ New Goal</button></div></div>
      <div class="overview-grid">
        <section class="light-panel"><div class="panel-heading"><strong>◈ OBJECT LIBRARY</strong><button class="pixel-button small" type="button" data-action="select-panel" data-panel="library">View all ${Number(state.gameObjectTotal)||0}</button></div>${recent.length ? `<div class="object-grid" style="padding:9px">${recent.map(renderObjectCard).join('')}</div>` : `<div class="empty-state" style="margin:10px"><div class="empty-icon">🧰</div><h3>YOUR LIBRARY IS EMPTY</h3><p>Create an object type with the fields you care about, then add your first object.</p><button class="pixel-button gold" type="button" data-action="new-type">＋ Create Object Type</button></div>`}</section>
        <div style="display:grid;align-content:start;gap:10px">
          <section class="light-panel"><div class="panel-heading"><strong>▣ YOUR SECTIONS</strong><button class="pixel-button small" type="button" data-action="new-section">＋ Add</button></div>${state.sections.filter((section) => !section.archived).length ? `<div class="section-list">${state.sections.filter((section) => !section.archived).map((section) => `<button class="section-row" type="button" data-action="select-section" data-id="${esc(section.id)}"><span class="section-ic">${renderIcon(section.icon)}</span><span>${esc(section.name)}</span><span class="section-count">${Number(section.object_count) || 0}</span></button>`).join('')}</div>` : `<div class="empty-state" style="margin:9px"><h3>MAKE YOUR OWN NAVIGATION</h3><p>Sections are fully customizable and can be nested.</p></div>`}</section>
          <section class="light-panel"><div class="panel-heading"><strong>◎ GOALS & PLANS</strong><button class="pixel-button small" type="button" data-action="open-goals">View all</button></div>${state.goals.length ? `<div class="goal-list">${state.goals.slice(0,3).map(renderGoal).join('')}</div>` : `<div class="empty-state" style="margin:9px"><h3>NO GOALS YET</h3><p>Track a manual milestone, checklist, or field target.</p><button class="pixel-button small" type="button" data-action="new-goal">＋ Create Goal</button></div>`}</section>
        </div>
      </div>`;
  }

  function renderLibrary() {
    return `<div class="content-heading"><div><h2>◈ OBJECT LIBRARY</h2><p>Browse every active object, including objects without a section.</p></div><button class="pixel-button gold" type="button" data-action="new-object">＋ New Object</button></div>${renderObjectControls()}<div id="library-results">${renderLibraryResults()}</div>`;
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
    return `<div class="content-heading"><div><h2>${renderIcon(section.icon)} ${esc(section.name)}</h2><p>${linkedCount} linked object${linkedCount === 1 ? '' : 's'} · ${esc(section.display_style)} view</p></div><div class="toolbar"><button class="pixel-button" type="button" data-action="add-existing-goal">＋ Goal</button><button class="pixel-button gold" type="button" data-action="add-existing">＋ Add Existing</button><button class="pixel-button" type="button" data-action="new-object">＋ Create Object</button><button class="pixel-button soft" type="button" data-action="edit-section" data-id="${esc(section.id)}">✎</button><button class="pixel-button danger small" type="button" data-action="archive-section" data-id="${esc(section.id)}" title="Archive section">Archive</button></div></div>
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
    return `<div class="content-heading"><div><h2>⚙ YOUR CUSTOM DATA TYPES</h2><p>Fields belong to a type and stay editable on each object. Their type cannot be changed after creation.</p></div><button class="pixel-button gold" type="button" data-action="new-type">＋ New Type</button></div>
      ${types.length ? `<div class="schema-grid">${types.map((type) => `<article class="schema-card"><h3><span>${renderIcon(type.icon||'◈')}</span>${esc(type.name)} ${type.archived?'(archived)':''}</h3>${type.image?`<img class="type-image" src="${esc(type.image)}" alt="">`:''}<p>${esc(type.description || 'Custom objects in this game.')}</p><div class="field-editor-list">${(type.fields||[]).map((field) => `<div class="field-row ${field.archived?'field-archived':''}"><span class="field-icon">${renderIcon(field.icon||'✦')}</span><strong>${esc(field.name)}</strong><span class="field-kind">${esc(typeLabel(field.field_type))}</span>${field.archived?`<button class="pixel-button small" type="button" data-action="restore-field" data-id="${esc(field.id)}">Restore</button>`:`<button class="pixel-button small soft" type="button" data-action="edit-field" data-id="${esc(field.id)}">Edit</button><button class="pixel-button small danger" type="button" data-action="archive-field" data-id="${esc(field.id)}" title="Archive field; values are preserved">Archive</button>`}</div>`).join('')}</div><div class="toolbar" style="margin-top:9px">${type.archived?`<button class="pixel-button small" type="button" data-action="restore-type" data-id="${esc(type.id)}">Restore type</button>`:`<button class="pixel-button small" type="button" data-action="new-field" data-type="${esc(type.id)}">＋ Add Field</button><button class="pixel-button small" type="button" data-action="edit-type" data-id="${esc(type.id)}">Edit type</button><button class="pixel-button small danger" type="button" data-action="archive-type" data-id="${esc(type.id)}">Archive type</button>`}</div></article>`).join('')}</div>` : `<div class="empty-state"><div class="empty-icon">🧩</div><h3>DESIGN YOUR OWN DATA</h3><p>Create a type and define characteristics that make sense for your game. No game comes with preset fields.</p><button class="pixel-button gold" type="button" data-action="new-type">＋ Create Object Type</button></div>`}`;
  }

  function renderWelcome() {
    return `<div class="game-hero"><div class="hero-symbol">✧</div><div class="hero-copy"><h1 class="hero-title">YOUR PERSONAL GAME TRACKER</h1><p class="hero-description">A cozy place for every build, quest, collection, and idea.</p></div></div><div class="content"><div class="empty-state" style="padding:48px 18px"><div class="empty-icon">🎮 ✦</div><h3>YOUR ADVENTURE BEGINS HERE</h3><p>Create your first game. Every section, object type, field, and goal after that is yours to define.</p><button class="pixel-button gold" type="button" data-action="new-game">＋ Add Your First Game</button></div></div>`;
  }

  return { renderPanel, renderOverview, renderLibrary, renderLibraryResults, refreshLibraryResults, renderSection, renderSchema, renderWelcome };
}
