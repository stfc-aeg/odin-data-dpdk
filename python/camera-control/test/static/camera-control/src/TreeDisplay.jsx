import React from 'react';

import 'bootstrap/dist/css/bootstrap.min.css';
import Container from 'react-bootstrap/Container';
import Row from 'react-bootstrap/Row';
import Col from 'react-bootstrap/Col';
import Form from 'react-bootstrap/Form';
import Card from 'react-bootstrap/Card';
import Alert from 'react-bootstrap/Alert';
import ListGroup from 'react-bootstrap/ListGroup';

import { WithEndpoint } from 'odin-react';

const EndpointInput = WithEndpoint(Form.Control)

function getMetadataByPath(metadata, path) {
    const keys = path.split('/').slice(1);
    // let current = metadata;
    // for (const key of keys) {
    //     if (current && typeof current === 'object') {
    //         current = current[key];
    //     } else {
    //         return null;
    //     }
    // }
    // return current;
    return keys.reduce((acc, key) => (acc && acc[key] !== undefined ? acc[key] : undefined), metadata);
}

function getDataByPath(data, path) {
    const keys = path.split('/');
    return keys.reduce((acc, key) => (acc && acc[key] !== undefined ? acc[key] : undefined), data);
}

function ParameterInput({ writeable, type, value, endpoint , fullpath}) {
    if (writeable === true) {
        if (type === "int" || type === "float") {
            return (
                <Container>
                    <EndpointInput
                        className="m-0 p-2"
                        endpoint={endpoint}
                        event_type="change"
                        type="number"
                        fullpath={fullpath} />
                </Container>
            );
        } if (type === "str") {
            return (
                <Container>
                    <EndpointInput
                        className="m-0 p-2"
                        endpoint={endpoint}
                        event_type="change"
                        type="string"
                        fullpath={fullpath} />
                </Container>
            );
        // Add option for boolean!
        } else {
            return (
                <Container>
                    <Alert className="m-0 p-2" variant={'danger'}>Unsupported type: {type}</Alert>
                </Container>
            );
        }
    } else {
        return (
            <Container>
                <p>{getDataByPath(endpoint.data, fullpath)}</p>
            </Container>
        )
    }
}

function Parameter({ data, metadata, endpoint, basepath, relpath, parentKey}) {
    if (Array.isArray(data)) {
        return (
            <Card className="px-0">
                <Card.Header>{parentKey.split("_").map((word, i) => i === 0 ? word.charAt(0).toUpperCase() + word.slice(1) : word).join(" ")}</Card.Header>
                <ListGroup variant="flush" className="px-0">
                {data.map((item, index) => {
                    const currentRelpath = `${relpath}/${index}`;
                    return (
                    <ListGroup.Item key={currentRelpath} className="px-2">
                        {typeof item === 'object' && item !== null ? (
                        <Parameter
                            data={item}
                            metadata={metadata}
                            endpoint={endpoint}
                            basepath={basepath}
                            relpath={currentRelpath}
                            parentKey={`${parentKey}_${index}`} />
                        ) : (
                        <Container>{item.toString()}</Container>
                        )}
                    </ListGroup.Item>
                    );
                })}
                </ListGroup>
            </Card>
        );
    } else if (typeof data === 'object' && data !== null) {
        return (
            <Card className="px-0">
                <Card.Header>{parentKey.split("_").map((word, i) => i === 0 ? word.charAt(0).toUpperCase() + word.slice(1) : word).join(" ")}</Card.Header>
                <ListGroup variant="flush" className="px-0">
                    {Object.entries(data).map(([key, value]) => {
                        const currentRelpath = `${relpath}/${key}`;
                        return (
                            <ListGroup.Item key={currentRelpath} className="px-2">
                                <Parameter
                                    data={value}
                                    metadata={metadata}
                                    endpoint={endpoint}
                                    basepath={basepath}
                                    relpath={currentRelpath}
                                    parentKey={key} />
                            </ListGroup.Item>
                        );
                    })}
                </ListGroup>
            </Card>
        );
    } else {
        return (
            <Container>
                <Row>
                    <Col>
                        <p>{parentKey.split("_").map((word, i) => i === 0 ? word.charAt(0).toUpperCase() + word.slice(1) : word).join(" ")}</p>
                    </Col>
                    <Col>
                        <ParameterInput
                            writeable={getMetadataByPath(metadata, relpath)?.writeable}
                            type={getMetadataByPath(metadata, relpath)?.type}
                            value={getMetadataByPath(metadata, relpath)?.value}
                            endpoint={endpoint}
                            fullpath={`${basepath}${relpath}`}/>
                    </Col>
                </Row>
            </Container>
        );
    }
}

function TreeDisplay(props) {
    const {endpoint, metadata, data, path, paths} = props;

    //could get data and metadata params from endpoint and path?

    return (
        <Container>
            <Row>
                <p></p>
                {paths && paths.length > 0 ? (
                    <Card className="px-0">
                        <ListGroup variant="flush">
                            {paths.map((p) => {
                                const value = getDataByPath(data, p);
                                return (
                                    <ListGroup.Item className="px-2">
                                        <Parameter
                                            key={p}
                                            data={value}
                                            metadata={metadata}
                                            endpoint={endpoint}
                                            basepath={path}
                                            relpath={p.replace(path, "")}
                                            parentKey={p.split("/").slice(-1)[0]}
                                        />
                                    </ListGroup.Item>
                                );
                            })}
                        </ListGroup>
                    </Card>
                ) : data ? (
                    <Parameter
                        data={data}
                        metadata={metadata}
                        endpoint={endpoint}
                        basepath={path}
                        relpath=""
                        parentKey={path.split("/").slice(-1)[0]}
                    />
                ) : (
                    <p></p>
                )}
            </Row>
        </Container>
    )
}

export default TreeDisplay;