package main

import (
	"regexp"
	"strconv"

	. "fd.io/hs-test/infra"
)

func init() {
	RegisterVethTests(EchoBuiltinTest, EchoBuiltinBandwidthTest, EchoBuiltinEchobytesTest, EchoBuiltinRoundtripTest)
	RegisterSoloVethTests(TcpWithLossTest)
	RegisterVeth6Tests(TcpWithLoss6Test)
}

func EchoBuiltinTest(s *VethsSuite) {
	serverVpp := s.Containers.ServerVpp.VppInstance

	serverVpp.Vppctl("test echo server " +
		" uri tcp://" + s.Interfaces.Server.Ip4AddressString() + "/" + s.Ports.Port1)

	clientVpp := s.Containers.ClientVpp.VppInstance

	o := clientVpp.Vppctl("test echo client nclients 100 bytes 1 verbose" +
		" syn-timeout 100 test-timeout 100" +
		" uri tcp://" + s.Interfaces.Server.Ip4AddressString() + "/" + s.Ports.Port1)
	s.Log(o)
	s.AssertNotContains(o, "failed:")
}

func EchoBuiltinBandwidthTest(s *VethsSuite) {
	regex := regexp.MustCompile(`gbytes\) in (\d+\.\d+) seconds`)
	serverVpp := s.Containers.ServerVpp.VppInstance

	serverVpp.Vppctl("test echo server " +
		" uri tcp://" + s.Interfaces.Server.Ip4AddressString() + "/" + s.Ports.Port1)

	clientVpp := s.Containers.ClientVpp.VppInstance

	o := clientVpp.Vppctl("test echo client nclients 4 bytes 8m throughput 16m" +
		" uri tcp://" + s.Interfaces.Server.Ip4AddressString() + "/" + s.Ports.Port1)
	s.Log(o)
	s.AssertContains(o, "Test started")
	s.AssertContains(o, "Test finished")
	if regex.MatchString(o) {
		matches := regex.FindStringSubmatch(o)
		if len(matches) != 0 {
			seconds, _ := strconv.ParseFloat(matches[1], 32)
			// Make sure that we are within 0.1 of the targeted
			// 2 seconds of runtime
			s.AssertEqualWithinThreshold(seconds, 2, 0.1)
		} else {
			s.AssertEmpty("invalid echo test client output")
		}
	} else {
		s.AssertEmpty("invalid echo test client output")
	}
}

func EchoBuiltinRoundtripTest(s *VethsSuite) {
	regex := regexp.MustCompile(`(\.\d+)ms roundtrip`)
	serverVpp := s.Containers.ServerVpp.VppInstance

	serverVpp.Vppctl("test echo server " +
		" uri tcp://" + s.Interfaces.Server.Ip4AddressString() + "/" + s.Ports.Port1)

	clientVpp := s.Containers.ClientVpp.VppInstance

	o := clientVpp.Vppctl("test echo client bytes 8m" +
		" uri tcp://" + s.Interfaces.Server.Ip4AddressString() + "/" + s.Ports.Port1)
	s.Log(o)
	s.AssertContains(o, "Test started")
	s.AssertContains(o, "Test finished")
	if regex.MatchString(o) {
		matches := regex.FindStringSubmatch(o)
		if len(matches) != 0 {
			seconds, _ := strconv.ParseFloat(matches[1], 32)
			// Make sure that we are within ms range
			s.AssertEqualWithinThreshold(seconds, 0.5, 0.5)
		} else {
			s.AssertEmpty("invalid echo test client output")
		}
	} else {
		s.AssertEmpty("invalid echo test client output")
	}
}

func EchoBuiltinEchobytesTest(s *VethsSuite) {
	serverVpp := s.Containers.ServerVpp.VppInstance

	serverVpp.Vppctl("test echo server " +
		" uri udp://" + s.Interfaces.Server.Ip4AddressString() + "/" + s.Ports.Port1)

	clientVpp := s.Containers.ClientVpp.VppInstance

	o := clientVpp.Vppctl("test echo client echo-bytes verbose uri" +
		" udp://" + s.Interfaces.Server.Ip4AddressString() + "/" + s.Ports.Port1)
	s.Log(o)
	s.AssertNotContains(o, "test echo clients: failed: timeout with 1 sessions")
}

func TcpWithLossTest(s *VethsSuite) {
	serverVpp := s.Containers.ServerVpp.VppInstance

	serverVpp.Vppctl("test echo server uri tcp://%s/"+s.Ports.Port1,
		s.Interfaces.Server.Ip4AddressString())

	clientVpp := s.Containers.ClientVpp.VppInstance

	// Add loss of packets with Network Delay Simulator
	clientVpp.Vppctl("set nsim poll-main-thread delay 0.01 ms bandwidth 40 gbit" +
		" packet-size 1400 packets-per-drop 1000")

	clientVpp.Vppctl("nsim output-feature enable-disable host-" + s.Interfaces.Server.Name())

	// Do echo test from client-vpp container
	output := clientVpp.Vppctl("test echo client uri tcp://%s/%s verbose echo-bytes bytes 50m",
		s.Interfaces.Server.Ip4AddressString(), s.Ports.Port1)
	s.Log(output)
	s.AssertNotEqual(len(output), 0)
	s.AssertNotContains(output, "failed", output)
}

func TcpWithLoss6Test(s *Veths6Suite) {
	serverVpp := s.Containers.ServerVpp.VppInstance

	serverVpp.Vppctl("test echo server uri tcp://%s/%s",
		s.Interfaces.Server.Ip6AddressString(), s.Ports.Port1)

	clientVpp := s.Containers.ClientVpp.VppInstance

	// Add loss of packets with Network Delay Simulator
	clientVpp.Vppctl("set nsim poll-main-thread delay 0.01 ms bandwidth 40 gbit" +
		" packet-size 1400 packets-per-drop 1000")

	clientVpp.Vppctl("nsim output-feature enable-disable host-" + s.Interfaces.Server.Name())

	// Do echo test from client-vpp container
	output := clientVpp.Vppctl("test echo client uri tcp://%s/%s verbose echo-bytes bytes 50m",
		s.Interfaces.Server.Ip6AddressString(), s.Ports.Port1)
	s.Log(output)
	s.AssertNotEqual(len(output), 0)
	s.AssertNotContains(output, "failed", output)
}
