# Build the workload programs (Go) into workloads/bin/.
#   make -C workloads         build grpcserver and loadgen
#   make -C workloads proto   regenerate kairospb/ from proto/kairos.proto
#                             (needs protoc, protoc-gen-go, protoc-gen-go-grpc)
#
# The generated kairospb/ files are committed, so a plain build needs only Go.

GO     ?= go
MODULE := github.com/AayushNair10/KairOS/workloads
# protoc finds its plugins (protoc-gen-go, protoc-gen-go-grpc) on PATH.
PLUGIN_DIR := $(shell $(GO) env GOPATH)/bin

# go build tracks its own dependencies, so always run it.
all:
	$(GO) build -o bin/ ./grpcserver ./loadgen

proto:
	PATH="$(PLUGIN_DIR):$$PATH" protoc -I proto			\
		--go_out=. --go_opt=module=$(MODULE)			\
		--go-grpc_out=. --go-grpc_opt=module=$(MODULE)		\
		proto/kairos.proto

clean:
	rm -rf bin

.PHONY: all proto clean
