// Redis mode (-redis): the same open-loop schedule, but each request is one
// GET of a random key on one of several Redis servers (RESEARCH_LOG
// section 5.1, experiment G: a real service instead of grpcserver).
//
// Each connection is pipelined: a request is written at its send time
// whether or not earlier ones were answered (open loop), and Redis answers
// the commands of one connection in order, so the n-th reply belongs to
// the n-th request written. The sender writes the command and queues the
// request's id under one lock, so the two orders agree; a reader goroutine
// per connection takes ids from that queue as replies arrive.
package main

import (
	"bufio"
	"errors"
	"fmt"
	"math/rand/v2"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"

	"google.golang.org/grpc/codes"
)

// maxPending bounds the requests in flight on one connection; beyond it the
// sender blocks (the server would have stalled for many seconds).
const maxPending = 1 << 16

type redisConn struct {
	mu   sync.Mutex
	conn net.Conn
	w    *bufio.Writer
	r    *bufio.Reader
	pend chan int // ids of requests written and not yet answered, in order
}

var errBadReply = errors.New("redis: malformed reply")

// readReply reads one RESP2 reply and returns an error for an error reply
// ("-ERR ...") or a broken connection. The value itself is discarded.
func readReply(r *bufio.Reader) error {
	line, err := r.ReadSlice('\n')
	if err != nil {
		return err
	}
	if len(line) < 3 || line[len(line)-2] != '\r' {
		return errBadReply
	}
	body := string(line[1 : len(line)-2])
	switch line[0] {
	case '+', ':':
		return nil
	case '-':
		return fmt.Errorf("redis: %s", body)
	case '$': // bulk string: length, then the bytes and \r\n; -1 = nil
		n, err := strconv.Atoi(body)
		if err != nil {
			return errBadReply
		}
		if n >= 0 {
			_, err = r.Discard(n + 2)
		}
		return err
	case '*': // array: that many replies follow
		n, err := strconv.Atoi(body)
		if err != nil {
			return errBadReply
		}
		for i := 0; i < n; i++ {
			if err := readReply(r); err != nil {
				return err
			}
		}
		return nil
	}
	return errBadReply
}

// writeCmd writes a command as a RESP array of bulk strings.
func writeCmd(w *bufio.Writer, args ...string) {
	fmt.Fprintf(w, "*%d\r\n", len(args))
	for _, a := range args {
		fmt.Fprintf(w, "$%d\r\n%s\r\n", len(a), a)
	}
}

func dialRedis(addr string) (*redisConn, error) {
	c, err := net.Dial("tcp", addr)
	if err != nil {
		return nil, err
	}
	rc := &redisConn{conn: c, w: bufio.NewWriter(c), r: bufio.NewReaderSize(c, 64<<10),
		pend: make(chan int, maxPending)}
	// warmup: one round trip before the measurement
	writeCmd(rc.w, "PING")
	if err := rc.w.Flush(); err != nil {
		return nil, err
	}
	if err := readReply(rc.r); err != nil {
		return nil, fmt.Errorf("%s: %v", addr, err)
	}
	return rc, nil
}

// redisClient spreads requests over conns connections to each address:
// request i goes to address i mod A, and to that address's connections in
// turn. Keys are drawn from key:0 .. key:keys-1 (DEBUG POPULATE's names).
type redisClient struct {
	addrs [][]*redisConn
	keys  int
	rng   *rand.Rand // used by the sender thread only
}

func newRedisClient(addrList string, conns, keys int, seed uint64) (*redisClient, error) {
	rc := &redisClient{keys: keys, rng: rand.New(rand.NewPCG(seed, 1))}
	for _, a := range strings.Split(addrList, ",") {
		var cs []*redisConn
		for range conns {
			c, err := dialRedis(strings.TrimSpace(a))
			if err != nil {
				return nil, err
			}
			cs = append(cs, c)
		}
		rc.addrs = append(rc.addrs, cs)
	}
	return rc, nil
}

// start runs one reader per connection. Each marks its requests done in
// recs (wg.Done per reply). A reader that hits an error, or waits longer
// than timeout for a reply, marks its remaining requests as failed.
func (rc *redisClient) start(t0 time.Time, recs []record, wg *sync.WaitGroup, timeout time.Duration) {
	for _, cs := range rc.addrs {
		for _, c := range cs {
			go func() {
				var dead error
				for id := range c.pend {
					if dead == nil {
						c.conn.SetReadDeadline(time.Now().Add(timeout))
						dead = readReply(c.r)
					}
					recs[id].doneNs = int64(time.Since(t0))
					if dead != nil {
						recs[id].code = codes.Unavailable
					}
					wg.Done()
				}
			}()
		}
	}
}

// send writes request id's GET on its connection; called on the sender
// thread at the request's send time.
func (rc *redisClient) send(id int) {
	cs := rc.addrs[id%len(rc.addrs)]
	c := cs[(id/len(rc.addrs))%len(cs)]
	key := "key:" + strconv.Itoa(rc.rng.IntN(rc.keys))
	c.mu.Lock()
	writeCmd(c.w, "GET", key)
	// A failed write means a broken connection; the reader then fails
	// to read this reply and marks the request (and the rest) failed.
	c.w.Flush()
	c.pend <- id
	c.mu.Unlock()
}

// stop ends the readers once every request has been answered.
func (rc *redisClient) stop() {
	for _, cs := range rc.addrs {
		for _, c := range cs {
			close(c.pend)
			c.conn.Close()
		}
	}
}
