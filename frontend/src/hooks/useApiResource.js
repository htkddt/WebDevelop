import { useEffect, useState } from 'react';

export default function useApiResource(loadResource, fallbackData) {
  const [data, setData] = useState(fallbackData);
  const [loading, setLoading] = useState(true);
  const [connected, setConnected] = useState(true);

  useEffect(() => {
    let isCurrent = true;

    async function load() {
      try {
        const response = await loadResource();
        if (isCurrent) {
          setData(response);
          setConnected(true);
        }
      } catch (error) {
        console.error('Backend connection error:', error);
        if (isCurrent) {
          setData(fallbackData);
          setConnected(false);
        }
      } finally {
        if (isCurrent) {
          setLoading(false);
        }
      }
    }

    load();
    return () => {
      isCurrent = false;
    };
  }, [fallbackData, loadResource]);

  return { data, loading, connected };
}
