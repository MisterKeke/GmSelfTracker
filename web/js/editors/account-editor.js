import { getItems } from '../api.js';
import { ensureDialog } from '../modal.js';
import { esc, renderIcon } from '../utils/formatting.js';

export function createAccountEditor({ openForm, fieldRow, state, root }) {
  async function openProfile() {
    const images=await getItems('/api/assets');
    const usedBytes=images.reduce((sum,image)=>sum+image.size_bytes,0);
    const imageManager=`<section class="form-field wide settings-section"><h3>Uploaded Images</h3><small>${images.length} / 1000 images · ${(usedBytes/1024/1024).toFixed(1)} / 250 MB. Images in use cannot be deleted.</small><div class="asset-list">${images.map((image)=>`<div class="section-row">${image.pending_deletion?'<span>Cleanup pending</span>':`<img class="custom-icon" src="${esc(image.url)}" alt="Uploaded image" loading="lazy">`}<small>${Math.ceil(image.size_bytes/1024)} KB</small><button class="pixel-button small danger" type="button" data-action="delete-image" data-id="${esc(image.id)}">${image.pending_deletion?'Retry cleanup':'Delete'}</button></div>`).join('')}</div></section>`;
    const allGames=await getItems('/api/games?archived=true');
    const archivedGames=allGames.filter((item)=>item.archived);
    const activeGames=allGames.filter((item)=>!item.archived);
    const [objectsByGame,sectionsByGame]=await Promise.all([
      Promise.all(activeGames.map(async game=>({game,items:await getItems(`/api/games/${encodeURIComponent(game.id)}/objects?archived=true`)}))),
      Promise.all(activeGames.map(async game=>({game,items:await getItems(`/api/games/${encodeURIComponent(game.id)}/sections?archived=true`)}))),
    ]);
    const archivedItems=[
      ...archivedGames.map((item)=>`<div class="section-row"><span>🎮</span><span>${esc(item.name)}</span><button type="button" class="pixel-button small" data-action="restore-game" data-id="${esc(item.id)}">Restore</button></div>`),
      ...sectionsByGame.flatMap(({game,items})=>items.filter(item=>item.archived).map(item=>`<div class="section-row"><span>${renderIcon(item.icon)}</span><span>${esc(item.name)} <small>section · ${esc(game.name)}</small></span><button type="button" class="pixel-button small" data-action="restore-section" data-id="${esc(item.id)}">Restore</button></div>`)),
      ...objectsByGame.flatMap(({game,items})=>items.filter(item=>item.archived).map(item=>`<div class="section-row"><span>◈</span><span>${esc(item.name)} <small>${esc(item.type_name)} · ${esc(game.name)}</small></span><button type="button" class="pixel-button small" data-action="restore-object" data-id="${esc(item.id)}">Restore</button></div>`)),
    ];
    const archives=archivedItems.length?`<div class="section-list light-panel">${archivedItems.join('')}</div>`:`<p class="dialog-help">There is no archived data to restore right now.</p>`;
    const account=`<section class="form-field wide settings-section"><h3>Account</h3><p><strong>Username:</strong> ${esc(state.user.username)}</p><p class="dialog-help">Your account and game data are stored in the local SQLite database.</p></section>`;
    const security=`<section class="form-field wide settings-section"><h3>Security</h3><p class="dialog-help">Changing your password revokes other active sessions.</p>${fieldRow('Current password','current_password','','password',true,'required autocomplete="current-password"')}${fieldRow('New password','password','','password',true,'required minlength="10" maxlength="72" autocomplete="new-password"')}${fieldRow('Confirm new password','confirm_password','','password',true,'required minlength="10" maxlength="72" autocomplete="new-password"')}</section>`;
    const archived=`<section class="form-field wide settings-section"><h3>Archived Data</h3><p class="dialog-help">Restore games, sections, or objects from any active game.</p>${archives}</section>`;
    openForm('PROFILE & SETTINGS',`${account}${security}${archived}${imageManager}`, 'SAVE SECURITY SETTINGS');
    ensureDialog(root).querySelector('#modal-form').dataset.form='change-password';
  }

  return { openProfile };
}
