import { esc, renderIcon } from '../utils/formatting.js';

export function createNavigation({ renderSectionTree, renderPanel, state }) {
  function renderGameList() {
    const games = state.games.filter((game) => game.name.toLowerCase().includes(state.search.toLowerCase()));
    if (!games.length) return `<p class="search-empty">${state.search ? 'No game matches.' : 'Your library is waiting.'}</p>`;
    return games.map((game, index) => `<button type="button" class="game-link ${state.game?.id === game.id ? 'active' : ''}" data-action="select-game" data-id="${esc(game.id)}"><span class="game-symbol">${renderIcon(game.icon||'🎮')}</span><span class="game-link-title">${esc(game.name)}</span></button>`).join('');
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
        ${tab('library','◈ Objects')}${tab('goals','◎ Goals')}${tab('schema','⚙ Types & Fields')}
      </nav>
      <div class="workspace-body">${renderSectionTree()}<section class="content">${renderPanel()}</section></div>`;
  }

  function tab(panel, label) { return `<button class="tab ${state.panel === panel ? 'active' : ''}" type="button" data-action="select-panel" data-panel="${panel}">${label}</button>`; }

  return { renderGameList, renderGame, tab };
}
