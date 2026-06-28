// Mock Pyroscope receiver for integration testing.
// Listens on :4040, accepts POST /ingest, writes received body to /tmp/pyroscope_mock.log
//
// Usage: go run tests/pyroscope_mock.go

package main

import (
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strings"
)

func main() {
	os.Remove("/tmp/pyroscope_mock.log")

	http.HandleFunc("/ingest", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", 405)
			return
		}
		body, err := io.ReadAll(r.Body)
		if err != nil {
			http.Error(w, "read error", 500)
			return
		}
		f, err := os.OpenFile("/tmp/pyroscope_mock.log", os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
		if err != nil {
			http.Error(w, "log error", 500)
			return
		}
		fmt.Fprintf(f, "REQUEST %s\nQUERY %s\nBODY_LEN %d\n%s\n---\n",
			r.URL.Path, r.URL.RawQuery, len(body), strings.TrimSpace(string(body)))
		f.Close()
		w.WriteHeader(200)
	})

	log.Println("mock Pyroscope listening on :4040")
	log.Fatal(http.ListenAndServe(":4040", nil))
}
