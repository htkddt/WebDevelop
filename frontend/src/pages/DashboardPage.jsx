import PageContent from '../components/PageContent';
import useApiResource from '../hooks/useApiResource';
import { dashboardApi } from '../services/api';
import DashboardView from '../views/DashboardView';

const FALLBACK_STATS = { dept: -1, total: -1, active: -1, inactive: -1 };

const DashboardPage = () => {
  const { data: stats, loading, connected } = useApiResource(
    dashboardApi.getStats,
    FALLBACK_STATS,
  );

  return (
    <PageContent>
      <DashboardView stats={stats} loading={loading} connected={connected} />
    </PageContent>
  );
};

export default DashboardPage;
