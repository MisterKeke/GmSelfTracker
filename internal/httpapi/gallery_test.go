package httpapi

import (
	"bytes"
	"encoding/json"
	"image"
	"image/png"
	"net/http"
	"testing"
)

func uploadTestPNG(t *testing.T, user *testUser, filename string) string {
	t.Helper()
	var content bytes.Buffer
	if err := png.Encode(&content, image.NewRGBA(image.Rect(0, 0, 1, 1))); err != nil {
		t.Fatal(err)
	}
	status, body := user.multipart("/api/assets", content.Bytes(), filename)
	if status != http.StatusCreated {
		t.Fatalf("upload image: status=%d body=%s", status, body)
	}
	return mapString(decodeMap(t, body), "id")
}

func TestObjectGalleryCaptionOrderPrimaryReuseAndCleanup(t *testing.T) {
	f := newImprovementFixture(t)
	assetID := uploadTestPNG(t, f.u, "gallery.png")
	path := "/api/objects/" + f.object + "/gallery"
	first := decodeMap(t, f.u.expect("POST", path, map[string]any{"asset_id": assetID, "caption": "First"}, http.StatusCreated))
	firstID := mapString(first, "id")
	if first["is_primary"] != true {
		t.Fatal("first gallery image should be primary by default")
	}
	second := decodeMap(t, f.u.expect("POST", path, map[string]any{"asset_id": assetID, "caption": "Second"}, http.StatusCreated))
	secondID := mapString(second, "id")
	if second["is_primary"] != false {
		t.Fatal("later gallery image should not replace the primary")
	}
	f.u.expect("PATCH", "/api/object-gallery/"+firstID, map[string]any{"sort_order": 10}, http.StatusOK)
	f.u.expect("PATCH", "/api/object-gallery/"+secondID, map[string]any{"caption": "Edited caption", "sort_order": 0, "is_primary": true}, http.StatusOK)
	var result struct {
		Items []objectGalleryImage `json:"items"`
	}
	if err := json.Unmarshal(f.u.expect("GET", path, nil, http.StatusOK), &result); err != nil {
		t.Fatal(err)
	}
	if len(result.Items) != 2 || result.Items[0].ID != secondID || !result.Items[0].IsPrimary || result.Items[0].Caption != "Edited caption" {
		t.Fatalf("gallery edit/order/primary did not persist: %#v", result.Items)
	}
	f.u.expect("DELETE", "/api/object-gallery/"+secondID, nil, http.StatusNoContent)
	f.u.expect("DELETE", "/api/assets/"+assetID, nil, http.StatusConflict)
	f.u.expect("DELETE", "/api/object-gallery/"+firstID, nil, http.StatusNoContent)
	f.u.expect("DELETE", "/api/assets/"+assetID, nil, http.StatusNoContent)
}

func TestObjectGalleryArchivedProtectionAndOwnership(t *testing.T) {
	f := newImprovementFixture(t)
	assetID := uploadTestPNG(t, f.u, "archived-gallery.png")
	path := "/api/objects/" + f.object + "/gallery"
	image := decodeMap(t, f.u.expect("POST", path, map[string]any{"asset_id": assetID}, http.StatusCreated))
	f.u.expect("POST", "/api/objects/"+f.object+"/archive", map[string]any{}, http.StatusNoContent)
	var result struct {
		Archived bool                 `json:"archived"`
		Items    []objectGalleryImage `json:"items"`
	}
	if err := json.Unmarshal(f.u.expect("GET", path, nil, http.StatusOK), &result); err != nil {
		t.Fatal(err)
	}
	if !result.Archived || len(result.Items) != 1 {
		t.Fatalf("archived object gallery should remain readable: %#v", result)
	}
	f.u.expect("DELETE", "/api/object-gallery/"+mapString(image, "id"), nil, http.StatusConflict)
	other := f.e.client()
	other.register("gallery_outsider")
	other.expect("GET", path, nil, http.StatusNotFound)
	other.expect("DELETE", "/api/object-gallery/"+mapString(image, "id"), nil, http.StatusNotFound)
	// The gallery reference protects the upload after the object is archived.
	f.u.expect("DELETE", "/api/assets/"+assetID, nil, http.StatusConflict)
}
