package httpapi

import (
	"context"
	"net/http"
	"sort"
	"strings"
)

const maxRelationshipGraphNodes = 100
const maxRelationshipGraphEdges = 200

type relationshipGraphNode struct {
	ID               string   `json:"id"`
	Name             string   `json:"name"`
	ObjectTypeID     string   `json:"object_type_id"`
	TypeName         string   `json:"type_name"`
	Image            string   `json:"image"`
	Archived         bool     `json:"archived"`
	Depth            int      `json:"depth"`
	ReverseExpansion bool     `json:"reverse_expansion"`
	ParentIDs        []string `json:"parent_ids"`
}

type relationshipGraphEdge struct {
	Source    string `json:"source"`
	Target    string `json:"target"`
	FieldID   string `json:"field_id"`
	FieldName string `json:"field_name"`
	Reverse   bool   `json:"reverse"`
}

type relationshipGraph struct {
	RootID    string                  `json:"root_id"`
	Nodes     []relationshipGraphNode `json:"nodes"`
	Edges     []relationshipGraphEdge `json:"edges"`
	Depth     int                     `json:"depth"`
	Direction string                  `json:"direction"`
	Truncated bool                    `json:"truncated"`
}

func (a *API) getObjectGraph(w http.ResponseWriter, r *http.Request) {
	root, err := a.loadObjectAny(r, r.PathValue("objectID"))
	if err != nil {
		notFoundOrError(w, err, "Object")
		return
	}
	depth := queryInt(r, "depth", 1, 1, 4)
	maxNodes := queryInt(r, "max_nodes", 50, 2, maxRelationshipGraphNodes)
	direction := strings.ToLower(r.URL.Query().Get("direction"))
	if direction == "" {
		direction = "both"
	}
	if direction != "both" && direction != "outbound" {
		writeError(w, http.StatusBadRequest, "invalid_graph_direction", "Choose outbound or both relationship directions.")
		return
	}
	graph, err := a.expandRelationshipGraph(r.Context(), root.GameID, root.ID, depth, maxNodes, direction)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "graph_failed", "Could not load object relationships.")
		return
	}
	writeJSON(w, http.StatusOK, graph)
}

func (a *API) expandRelationshipGraph(ctx context.Context, gameID, rootID string, depthLimit, nodeLimit int, direction string) (relationshipGraph, error) {
	graph := relationshipGraph{RootID: rootID, Depth: depthLimit, Direction: direction, Nodes: []relationshipGraphNode{}, Edges: []relationshipGraphEdge{}}
	nodes := map[string]relationshipGraphNode{}
	edges := map[string]relationshipGraphEdge{}
	rootNode, err := a.loadRelationshipGraphNode(ctx, gameID, rootID, objectRecord{}, 0)
	if err != nil {
		return graph, err
	}
	rootNode.ParentIDs = []string{}
	nodes[rootID] = rootNode
	frontier := []string{rootID}
	visited := map[string]bool{rootID: true}
	for currentDepth := 0; currentDepth < depthLimit && len(frontier) > 0; currentDepth++ {
		outgoing, err := a.graphEdgesForNodes(ctx, gameID, frontier, false)
		if err != nil {
			return graph, err
		}
		incoming := []relationshipGraphEdge{}
		if direction == "both" {
			incoming, err = a.graphEdgesForNodes(ctx, gameID, frontier, true)
			if err != nil {
				return graph, err
			}
		}
		next := []string{}
		for _, candidate := range append(outgoing, incoming...) {
			neighborID := candidate.Target
			parentID := candidate.Source
			if candidate.Reverse {
				neighborID = candidate.Source
				parentID = candidate.Target
			}
			if _, ok := nodes[neighborID]; !ok {
				if len(nodes) >= nodeLimit {
					graph.Truncated = true
					continue
				}
				item, err := a.loadRelationshipGraphNode(ctx, gameID, neighborID, objectRecord{}, currentDepth+1)
				if err != nil {
					// A concurrently removed record should not leave a dangling node or edge.
					continue
				}
				item.ReverseExpansion = candidate.Reverse
				item.ParentIDs = []string{parentID}
				nodes[neighborID] = item
			} else {
				item := nodes[neighborID]
				if !containsString(item.ParentIDs, parentID) {
					item.ParentIDs = append(item.ParentIDs, parentID)
					nodes[neighborID] = item
				}
			}
			key := candidate.Source + ":" + candidate.FieldID + ":" + candidate.Target
			if old, ok := edges[key]; ok {
				if old.Reverse && !candidate.Reverse {
					old.Reverse = false
					edges[key] = old
				}
			} else if len(edges) < maxRelationshipGraphEdges {
				edges[key] = candidate
			} else {
				graph.Truncated = true
				continue
			}
			if currentDepth+1 < depthLimit && !nodes[neighborID].Archived && !visited[neighborID] {
				visited[neighborID] = true
				next = append(next, neighborID)
			}
		}
		frontier = uniqueSortedStrings(next)
	}
	for _, node := range nodes {
		graph.Nodes = append(graph.Nodes, node)
	}
	sortRelationshipNodes(graph.Nodes)
	for _, edge := range edges {
		graph.Edges = append(graph.Edges, edge)
	}
	sortRelationshipEdges(graph.Edges)
	return graph, nil
}

func containsString(values []string, target string) bool {
	for _, value := range values {
		if value == target {
			return true
		}
	}
	return false
}

func (a *API) graphEdgesForNodes(ctx context.Context, gameID string, ids []string, reverse bool) ([]relationshipGraphEdge, error) {
	if len(ids) == 0 {
		return nil, nil
	}
	placeholders := strings.TrimRight(strings.Repeat("?,", len(ids)), ",")
	query := `SELECT r.object_id,r.referenced_object_id,r.field_id,f.name FROM object_relationships r JOIN field_definitions f ON f.id=r.field_id WHERE r.game_id=? AND r.object_id IN (` + placeholders + `) ORDER BY r.object_id,r.field_id,r.referenced_object_id`
	if reverse {
		query = `SELECT r.object_id,r.referenced_object_id,r.field_id,f.name FROM object_relationships r JOIN field_definitions f ON f.id=r.field_id WHERE r.game_id=? AND r.referenced_object_id IN (` + placeholders + `) ORDER BY r.object_id,r.field_id,r.referenced_object_id`
	}
	args := make([]any, 0, len(ids)+1)
	args = append(args, gameID)
	for _, id := range ids {
		args = append(args, id)
	}
	rows, err := a.db.QueryContext(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	edges := []relationshipGraphEdge{}
	for rows.Next() {
		var edge relationshipGraphEdge
		if err := rows.Scan(&edge.Source, &edge.Target, &edge.FieldID, &edge.FieldName); err != nil {
			return nil, err
		}
		edge.Reverse = reverse
		edges = append(edges, edge)
	}
	return edges, rows.Err()
}

func (a *API) loadRelationshipGraphNode(ctx context.Context, gameID, id string, cached objectRecord, depth int) (relationshipGraphNode, error) {
	if cached.ID != "" {
		return relationshipGraphNode{ID: cached.ID, Name: cached.Name, ObjectTypeID: cached.ObjectTypeID, TypeName: cached.TypeName, Image: cached.Image, Archived: cached.Archived, Depth: depth}, nil
	}
	var node relationshipGraphNode
	var objectArchived, typeArchived int
	err := a.db.QueryRowContext(ctx, `SELECT o.id,o.name,o.object_type_id,t.name,o.image,o.archived,t.archived
		FROM objects o JOIN object_types t ON t.id=o.object_type_id WHERE o.id=? AND o.game_id=?`, id, gameID).Scan(&node.ID, &node.Name, &node.ObjectTypeID, &node.TypeName, &node.Image, &objectArchived, &typeArchived)
	node.Archived, node.Depth = objectArchived != 0 || typeArchived != 0, depth
	return node, err
}

func sortRelationshipNodes(nodes []relationshipGraphNode) {
	sort.Slice(nodes, func(i, j int) bool {
		if nodes[i].Depth != nodes[j].Depth {
			return nodes[i].Depth < nodes[j].Depth
		}
		return nodes[i].ID < nodes[j].ID
	})
}

func sortRelationshipEdges(edges []relationshipGraphEdge) {
	sort.Slice(edges, func(i, j int) bool {
		if edges[i].Source != edges[j].Source {
			return edges[i].Source < edges[j].Source
		}
		if edges[i].FieldID != edges[j].FieldID {
			return edges[i].FieldID < edges[j].FieldID
		}
		return edges[i].Target < edges[j].Target
	})
}
