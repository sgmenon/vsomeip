# Documentation index

## Guides

- [User guide](./vsomeipUserGuide.md)
- [Configuration reference](./vsomeipConfiguration.md)
- [Protocol (local IPC)](./vsomeipProtocol.md)
- [Network tests](./network-tests.md)

## Design notes (mainline candidates)

- [Standalone service discovery](./standalone-service-discovery.md)
- [E2E scatter-gather send](./e2e-scatter-gather-send.md)
- [Send-path copy elision](./send-copy-elision.md)
- [Receive-path copy elision](./receive-copy-elision.md)

## Working notes

- [Asio threads, mutexes, and strands](./asio-strands.md)

## Multicast

To use IP multicast, the route must be added. In Linux this can be done
by:

```
# route add -net 224.0.0.0/4 dev eth0
```

Other OSes may have different ways to do this.

## Diagrams

### Use cases Overview

![Use cases Overview](./diagrams/usecases_overview.drawio.svg)

### Use case Offer service

![Offer service](./diagrams/offer_service.drawio.svg)

### Sequence Offer service

![Seq Offer service](./diagrams/sequence_offer_service.puml.svg)

## Support and review

### Open and closed points

- [ ] update the EA model to match the current implementation
