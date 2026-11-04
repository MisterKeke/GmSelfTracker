export function createWorkspaceState(user) {
  return { user, games: [], game: null, sections: [], types: [], objects: [], goals: [], panel: 'overview', sectionId: null, search: '', objectPage: 0, expanded: new Set(), views: new Map() };
}
