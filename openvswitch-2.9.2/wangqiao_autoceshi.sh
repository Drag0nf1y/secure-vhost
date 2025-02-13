#!/bin/bash
ovs-vsctl del-br br0
ovs-vsctl add-br br0 -- set bridge br0 datapath_type=netdev
ovs-vsctl add-port br0 dpdk-p0 -- set Interface dpdk-p0 type=dpdk options:dpdk-devargs=0000:01:00.0
ovs-vsctl add-port br0 dpdk-p1 -- set Interface dpdk-p1 type=dpdk options:dpdk-devargs=0000:01:00.1
ovs-vsctl add-port br0 vhost-user-1 -- set Interface vhost-user-1 type=dpdkvhostuserclient options:vhost-server-path="/tmp/sock0"
ovs-vsctl add-port br0 vhost-user-2 -- set Interface vhost-user-2 type=dpdkvhostuserclient options:vhost-server-path="/tmp/sock1"
ovs-vsctl add-port br0 vhost-user-3 -- set Interface vhost-user-3 type=dpdkvhostuserclient options:vhost-server-path="/tmp/sock2"
ovs-vsctl add-port br0 vhost-user-4 -- set Interface vhost-user-4 type=dpdkvhostuserclient options:vhost-server-path="/tmp/sock3"
ovs-vsctl add-port br0 vhost-user-5 -- set Interface vhost-user-5 type=dpdkvhostuserclient options:vhost-server-path="/tmp/sock4"
ovs-vsctl add-port br0 vhost-user-6 -- set Interface vhost-user-6 type=dpdkvhostuserclient options:vhost-server-path="/tmp/sock5"
ovs-vsctl add-port br0 vhost-user-7 -- set Interface vhost-user-7 type=dpdkvhostuserclient options:vhost-server-path="/tmp/sock6"
ovs-vsctl add-port br0 vhost-user-8 -- set Interface vhost-user-8 type=dpdkvhostuserclient options:vhost-server-path="/tmp/sock7"
ovs-ofctl del-flows br0
ovs-ofctl add-flow br0 "in_port=1,dl_dst=00:00:00:00:00:01,actions=output:3"
ovs-ofctl add-flow br0 "in_port=1,dl_dst=00:00:00:00:00:02,actions=output:4"
ovs-ofctl add-flow br0 "in_port=1,dl_dst=00:00:00:00:00:03,actions=output:5"
ovs-ofctl add-flow br0 "in_port=1,dl_dst=00:00:00:00:00:04,actions=output:6"
ovs-ofctl add-flow br0 "in_port=1,dl_dst=00:00:00:00:00:05,actions=output:7"
ovs-ofctl add-flow br0 "in_port=1,dl_dst=00:00:00:00:00:06,actions=output:8"
ovs-ofctl add-flow br0 "in_port=1,dl_dst=00:00:00:00:00:07,actions=output:9"
ovs-ofctl add-flow br0 "in_port=1,dl_dst=00:00:00:00:00:08,actions=output:10"
ovs-ofctl add-flow br0 "in_port=3,actions=output:2"
ovs-ofctl add-flow br0 "in_port=4,actions=output:2"
ovs-ofctl add-flow br0 "in_port=5,actions=output:2"
ovs-ofctl add-flow br0 "in_port=6,actions=output:2"
ovs-ofctl add-flow br0 "in_port=7,actions=output:2"
ovs-ofctl add-flow br0 "in_port=8,actions=output:2"
ovs-ofctl add-flow br0 "in_port=9,actions=output:2"
ovs-ofctl add-flow br0 "in_port=10,actions=output:2"

